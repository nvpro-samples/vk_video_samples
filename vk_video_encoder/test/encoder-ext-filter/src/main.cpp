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
        // PLANE COUNT COMES FROM THE FORMAT, NOT FROM ITS CLASS. Answering it from
        // the class gives every VIA_FILTER format 3, because the 3-plane family
        // dominates it. RGBA
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
    //
    // |planes| is carried per row rather than asserted as a constant zero,
    // because plane count is NOT the same question. It is a fact about the
    // LAYOUT and carries no colour model: Y410 is one interleaved plane
    // whichever model is declared over it, and this library does route it
    // under a Y'CbCr declaration, so 0 there would describe an input no
    // allocation could be sized from. Everything else here is routed under no
    // declaration at all and has no plane count to give.
    struct Row { VkFormat format; uint32_t planes; };
    const Row unsupported[] = {
        { VK_FORMAT_R8G8B8A8_SRGB,            0 },
        { VK_FORMAT_B8G8R8A8_SRGB,            0 },
        { VK_FORMAT_A8B8G8R8_SRGB_PACK32,     0 },
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1 },   // Y410's enumerant
        { VK_FORMAT_R16G16B16A16_UNORM,       0 },   // Y416's, and unrouted
        { VK_FORMAT_R16G16B16A16_SFLOAT,      0 },
        { VK_FORMAT_R8G8B8_UNORM,             0 },   // 3-component, no alpha
        // YUY2. Refused on its LAYOUT, not its subsampling: it is one
        // interleaved plane the generator would read as two. Semi-planar
        // 4:2:2 at the same subsampling IS routed --
        // CaseSinglePlaneInterleavedIsRefused drives both halves.
        { VK_FORMAT_G8B8G8R8_422_UNORM,       0 },
        { VK_FORMAT_UNDEFINED,                0 },
    };
    for (const Row& row : unsupported) {
        const VkFormat f = row.format;
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) == VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              "classified UNSUPPORTED", "format " + U32((uint32_t)f));
        Check(VkEncSupportsInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) == VK_FALSE,
              "not supported", "format " + U32((uint32_t)f));
        Check(VkEncInputFormatPlaneCount(f) == row.planes,
              "plane count as the layout has it",
              "format " + U32((uint32_t)f) + " gave " +
                  U32(VkEncInputFormatPlaneCount(f)) + ", want " +
                  U32(row.planes));
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

// The advertised input-format list: every format this library can route to an
// encoder input the DEVICE accepts for the profile, each naming what it is
// encoded as. Driven with synthetic device lists because the advertisement is
// a pure function of one, and the interesting lists -- a format the library
// refuses, one format reported twice, a device with no reachable target for a
// routable input -- are not what any one device reports.
//
// NV12 is spelled out per case rather than hoisted, so a case that changes
// the device list changes it visibly.

// A named format, for assertion text. Not a public mapping: the advertisement
// carries enumerants and this only makes a failure readable.
const char* AdvName(VkFormat f)
{
    switch (f) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:                   return "NV12";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:  return "P010";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:  return "P012";
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:                   return "NV24";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:  return "S410";
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:                  return "I420";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: return "I420-10";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: return "I420-12";
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:                   return "NV16";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:  return "P210";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:  return "P212";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:  return "S412";
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:                  return "I422";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16: return "I422-10";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16: return "I422-12";
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:                  return "I444";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16: return "I444-10";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16: return "I444-12";
        case VK_FORMAT_R8G8B8A8_UNORM:                             return "RGBA8";
        case VK_FORMAT_B8G8R8A8_UNORM:                             return "BGRA8";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:                      return "ABGR8";
        default:                                                   return "?";
    }
}

// Check() takes a C string and the labels below are built per entry, so the
// built label has to outlive the argument list.
const char* Lbl(const std::string& text)
{
    static std::string held;
    held = text;
    return held.c_str();
}

// THE SYNTHETIC ADMISSION, and what it is for.
//
// VkEncAdvertiseInputFormats takes an admission callback rather than a device
// list, because the production caller is a LIVE per-candidate resolve against a
// real driver -- the same one the point query answers from, which is what stops
// the two surfaces drifting. The cases below are device-free, so they supply
// this instead: the rule the function used to contain, stated once here rather
// than twelve times in twelve lambdas.
//
// It reproduces exactly what the old device-list form did:
//   - ENCODABLE_DIRECT and on the device list  -> OPTIMAL, naming itself;
//   - ENCODABLE_VIA_FILTER with a conversion target the device list carries
//     -> SUBOPTIMAL, naming the target;
//   - anything else                            -> not advertised.
//
// WHAT IS UNDER TEST HERE IS EVERYTHING BUT THIS RULE: the ordering, the
// uniqueness, the capacity stop and the compute-filter build gate all live
// inside VkEncAdvertiseInputFormats and are exercised through this admission,
// so the assertions in the cases below are unchanged from when the rule was
// inside the function.
struct SyntheticDeviceList {
    const VkFormat* formats;
    uint32_t        count;
};

bool DeviceListHas(const VkFormat* formats, uint32_t count, VkFormat format)
{
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i] == format) {
            return true;
        }
    }
    return false;
}

bool AdmitFromSyntheticDeviceList(
    void* userData, VkFormat candidate,
    VkVideoEncoderInputFormatProperties* outEntry)
{
    const SyntheticDeviceList* const dev =
        static_cast<const SyntheticDeviceList*>(userData);
    const VkEncInputFormatClass cls =
        VkEncClassifyInput(candidate, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT);
    if (cls == VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT) {
        if (!DeviceListHas(dev->formats, dev->count, candidate)) {
            return false;
        }
        outEntry->format       = candidate;
        outEntry->encodeFormat = candidate;
        outEntry->optimality   = VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL;
        return true;
    }
    if (cls == VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER) {
        const VkFormat target =
            VkEncConversionTargetFormat(candidate, dev->formats, dev->count);
        if (target == VK_FORMAT_UNDEFINED) {
            return false;
        }
        if (!DeviceListHas(dev->formats, dev->count, target)) {
            return false;
        }
        outEntry->format       = candidate;
        outEntry->encodeFormat = target;
        outEntry->optimality   = VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL;
        return true;
    }
    return false;
}

uint32_t AdvertiseFromDeviceList(
    const VkFormat* deviceFormats, uint32_t deviceFormatCount,
    VkVideoEncoderInputFormatProperties* outEntries, uint32_t outCapacity)
{
    SyntheticDeviceList dev = { deviceFormats, deviceFormatCount };
    return VkEncAdvertiseInputFormats(&AdmitFromSyntheticDeviceList, &dev,
                                      outEntries, outCapacity);
}

// Index of |format| in the advertised list, or -1.
int32_t AdvIndexOf(const VkVideoEncoderInputFormatProperties* entries,
                   uint32_t count, VkFormat format)
{
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].format == format) {
            return (int32_t)i;
        }
    }
    return -1;
}

void CaseAdvertisedListDropsWhatTheLibraryRefuses()
{
    g_currentCase = "the advertised list drops what the library refuses";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,     // routed, direct
        VK_FORMAT_G8B8G8R8_422_UNORM,           // packed 4:2:2, refused
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,     // routed, direct
        VK_FORMAT_R16G16B16A16_SFLOAT,          // refused
    };
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 4, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);

    // The refusals are the point: neither may appear anywhere in the answer,
    // as an input format or as a conversion target.
    for (uint32_t i = 0; i < n; i++) {
        Check((out[i].format != VK_FORMAT_G8B8G8R8_422_UNORM) &&
                  (out[i].format != VK_FORMAT_R16G16B16A16_SFLOAT) &&
                  (out[i].encodeFormat != VK_FORMAT_G8B8G8R8_422_UNORM) &&
                  (out[i].encodeFormat != VK_FORMAT_R16G16B16A16_SFLOAT),
              "no refused format is advertised, on either side of an entry",
              "entry " + U32(i) + " = " + U32((uint32_t)out[i].format) + " -> " +
                  U32((uint32_t)out[i].encodeFormat));
    }
    // And the two the device DOES offer that the library routes unconverted
    // are still there, in the device's order -- without which a function that
    // answered nothing would pass the loop above.
    Check((n >= 2) && (out[0].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) &&
              (out[0].encodeFormat == out[0].format),
          "the first device format the library routes is direct and first",
          "got " + U32(n) + " entries");
    Check((n >= 2) && (out[1].format == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM) &&
              (out[1].encodeFormat == out[1].format),
          "the second is direct and second -- the device order is kept",
          "got " + U32(n) + " entries");
}

void CaseAdvertisedListPassesWhatTheLibraryRoutes()
{
    g_currentCase = "the advertised list keeps every format the library routes";
    // The positive control for the case above: an advertisement that answered
    // nothing would pass it just as well. Four direct formats, and every
    // converted entry whose target is among them.
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
    };
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 4, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    // Four direct entries always; the seven converted ones only where the
    // filter is compiled in, because the advertisement is gated on it.
    Check(n == (kFilterCompiledIn ? 11u : 4u),
          "four direct entries, and seven filtered ones where the build has "
          "the filter to honour them",
          "got " + U32(n));
    for (uint32_t i = 0; (i < n) && (i < 4); i++) {
        Check((out[i].format == deviceList[i]) &&
                  (out[i].encodeFormat == deviceList[i]) &&
                  (out[i].optimality ==
                   VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL),
              "the direct entries come first, in the device's order",
              "entry " + U32(i));
    }
    // Each 3-plane input converts into the semi-planar sibling at its own
    // subsampling and depth, and this device list carries all four of those
    // siblings; the three RGBA spellings convert into the device's first
    // choice. THE 4:4:4 PAIR IS HERE BECAUSE THE DEVICE LIST CARRIES NV24 AND
    // S410 -- take those two out of the list and both entries go with them,
    // which is what the 4:2:0 case below asserts.
    struct Expect { VkFormat in; VkFormat out; };
    const Expect converted[] = {
        { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
        { VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM },
        { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 },
        { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16 },
        { VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
        { VK_FORMAT_B8G8R8A8_UNORM,      VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
        { VK_FORMAT_A8B8G8R8_UNORM_PACK32,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
    };
    for (const Expect& e : converted) {
        const int32_t at = AdvIndexOf(out, n, e.in);
        if (!kFilterCompiledIn) {
            // The build cannot convert, so the session would refuse each of
            // these. Advertising them would be the advertise-then-refuse
            // case the gate exists to prevent, so absence IS the assertion.
            Check(at < 0,
                  Lbl(std::string(AdvName(e.in)) +
                      " is NOT advertised without the filter"),
                  "index " + U32((uint32_t)(at + 1)));
            continue;
        }
        Check(at >= 0, Lbl(std::string(AdvName(e.in)) + " is advertised"),
              "not in the list");
        if (at >= 0) {
            Check(out[at].encodeFormat == e.out,
                  Lbl(std::string(AdvName(e.in)) + " names " +
                      AdvName(e.out) + " as what it is encoded as"),
                  "names " + U32((uint32_t)out[at].encodeFormat));
            Check(out[at].optimality ==
                      VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL,
                  Lbl(std::string(AdvName(e.in)) +
                      " is advertised as filtered"),
                  "optimality " + U32((uint32_t)out[at].optimality));
        }
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
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 4, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    uint32_t nv12Entries = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
            nv12Entries++;
        }
    }
    Check(nv12Entries == 1,
          "a format the device reports three times is advertised once",
          "appears " + U32(nv12Entries) + " times");
    Check((n >= 2) && (out[0].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) &&
              (out[1].format ==
               VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16),
          "and the distinct direct entries keep the device's order", "");
}

void CaseAdvertisedListStopsAtCapacity()
{
    g_currentCase = "the advertised list never writes past its capacity";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
    };
    VkVideoEncoderInputFormatProperties out[4] = {};
    const uint32_t n = AdvertiseFromDeviceList(deviceList, 3, out, 2);
    Check(n == 2, "clamped to the capacity given", "got " + U32(n));
    Check((out[2].format == VK_FORMAT_UNDEFINED) &&
              (out[2].encodeFormat == VK_FORMAT_UNDEFINED) &&
              (out[2].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL),
          "and wrote nothing past it",
          "slot 2 = " + U32((uint32_t)out[2].format));
}

// THE MEMBERSHIP RULE THAT KEEPS A PRODUCER OUT OF A REFUSED SESSION.
//
// Two of the formats the taxonomy routes -- 12-bit I420 and P012 -- convert
// into P012. Where P012 is an encode source on no profile the device exposes,
// advertising either would put an entry a producer can size a pool from in
// front of a caller the session then refuses, so neither is advertised. Where
// the device DOES take P012, both are, and the difference is read from the
// device list rather than from a table in this library.
void CaseAdvertisedListDropsAnUnreachableConversionTarget()
{
    g_currentCase = "an entry whose conversion target the device will not "
                    "take is not advertised";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
    };
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 2, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    Check(AdvIndexOf(out, n,
                     VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16) < 0,
          "I420-12 is not advertised: it converts into P012, which this "
          "device list does not carry",
          "it is in the list");
    Check(AdvIndexOf(out, n,
                     VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16) < 0,
          "P012 is not advertised: it converts into P012, which this device "
          "list does not carry either",
          "it is in the list");

    // THE POSITIVE CONTROL, on the same function and the same shape: add P012
    // to what the device takes and BOTH entries that route into it appear.
    // P012 is one of them -- its route is the filter and its target is
    // itself, which the entry states in its optimality rather than leaving to
    // be inferred from two equal formats.
    const VkFormat twelveBitDevice[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
    };
    VkVideoEncoderInputFormatProperties wide[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t wn = AdvertiseFromDeviceList(
        twelveBitDevice, 3, wide, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    const int32_t at = AdvIndexOf(
        wide, wn, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16);
    // Only meaningful where converted entries are advertised at all; without
    // the filter the answer is the build's, not the device's, and the case
    // above already asserts that.
    Check(kFilterCompiledIn ? (at >= 0) : (at < 0),
          "on a device that DOES take P012, I420-12 is advertised -- the "
          "exclusion is the device's answer, not a hardcoded one (and it is "
          "absent altogether in a build without the filter)",
          "index " + U32((uint32_t)(at + 1)));
    if (at >= 0) {
        Check(wide[at].encodeFormat ==
                  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
              "and it names P012 as what it is encoded as",
              "names " + U32((uint32_t)wide[at].encodeFormat));
        Check(wide[at].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL,
              "and it is filtered", "optimality " +
                  U32((uint32_t)wide[at].optimality));
    }
    const int32_t p012 = AdvIndexOf(
        wide, wn, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16);
    // Same shape as the row above: P012 is a converted entry, so whether the
    // device takes its target only decides the answer in a build that can
    // convert at all.
    Check(kFilterCompiledIn ? (p012 >= 0) : (p012 < 0),
          "P012 is advertised there too: the device takes its conversion "
          "target, so it is a route -- and it is absent altogether where the "
          "build has no filter",
          "index " + U32((uint32_t)(p012 + 1)));
    if (p012 >= 0) {
        Check(wide[p012].encodeFormat == wide[p012].format,
              "P012 names itself as what it is encoded as",
              "names " + U32((uint32_t)wide[p012].encodeFormat));
        Check(wide[p012].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL,
              "and it is FILTERED, which is what the two equal formats "
              "cannot say",
              "optimality " + U32((uint32_t)wide[p012].optimality));
    }
}

// WHY THIS EXISTS. NV24 and S410 are the two strongest claims the taxonomy
// makes -- ENCODABLE_DIRECT, and on the routable list -- and nothing in this
// tree has ever encoded either. This case records exactly what stands between
// them and a caller, so the answer is measured rather than re-derived, and so
// a change to any of the three gates shows up here.
//
// The three gates, and which one actually blocks them:
//   1. the converted arm requires ENCODABLE_VIA_FILTER and they are DIRECT,
//      so that arm skips them and their routable-list entries are inert;
//   2. VkEncConversionTargetFormat names no target for them, so even a
//      VIA_FILTER classification would not hand them a route;
//   3. the OPTIMAL arm is the device list -- which is the ONLY way either can
//      be advertised, and so is where the block actually lives.
//
// CALIBRATION. The same call is driven with two device lists differing only
// in whether they carry 4:4:4, and it must answer differently: absent on the
// first, present on the second. A check that passed both would be asserting
// nothing about reachability at all.
void CaseFourFourFourReachesTheListOnlyFromTheDevice()
{
    g_currentCase = "4:4:4 is advertised only where the device reports it, "
                    "and never off the routable list";

    const VkFormat nv24 = VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
    const VkFormat s410 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16;

    // Gate 1: DIRECT, so the converted arm's VIA_FILTER test skips them.
    Check(VkEncClassifyInput(nv24, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
          "NV24 classifies DIRECT, so the converted arm cannot emit it",
          "class " + U32((uint32_t)VkEncClassifyInput(
              nv24, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT)));
    Check(VkEncClassifyInput(s410, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
              VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
          "S410 classifies DIRECT, likewise",
          "class " + U32((uint32_t)VkEncClassifyInput(
              s410, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT)));

    // Gate 2: and no conversion produces them either.
    const VkFormat narrow[] = { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM };
    Check(VkEncConversionTargetFormat(nv24, narrow, 1) == VK_FORMAT_UNDEFINED,
          "NV24 names no conversion target, so it has no filter route",
          "target " + U32((uint32_t)VkEncConversionTargetFormat(nv24, narrow, 1)));
    Check(VkEncConversionTargetFormat(s410, narrow, 1) == VK_FORMAT_UNDEFINED,
          "S410 names no conversion target either",
          "target " + U32((uint32_t)VkEncConversionTargetFormat(s410, narrow, 1)));

    // Gate 3, ABSENT HALF. A device reporting no 4:4:4 gets no 4:4:4 entry,
    // although both formats are on the routable list this walk covers.
    VkVideoEncoderInputFormatProperties without[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t wn = AdvertiseFromDeviceList(
        narrow, 1, without, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    Check(wn > 0, "the 4:2:0-only device still advertises something, so the "
                  "absence below is a filter and not an empty list",
          "got " + U32(wn));
    uint32_t found444 = 0;
    for (uint32_t i = 0; i < wn; i++) {
        if ((without[i].format == nv24) || (without[i].format == s410)) {
            found444++;
        }
    }
    Check(found444 == 0,
          "no 4:4:4 entry is advertised when the device reports none",
          "found " + U32(found444));

    // Gate 3, PRESENT HALF. The same call on a device that does report them
    // advertises both, so the advertisement is not what withholds them.
    const VkFormat wide444[] = { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, nv24, s410 };
    VkVideoEncoderInputFormatProperties withList[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t gn = AdvertiseFromDeviceList(
        wide444, 3, withList, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    int nv24At = -1;
    int s410At = -1;
    for (uint32_t i = 0; i < gn; i++) {
        if (withList[i].format == nv24) { nv24At = (int)i; }
        if (withList[i].format == s410) { s410At = (int)i; }
    }
    Check(nv24At >= 0, "NV24 IS advertised once the device reports it",
          "index " + U32((uint32_t)(nv24At + 1)));
    Check(s410At >= 0, "S410 IS advertised once the device reports it",
          "index " + U32((uint32_t)(s410At + 1)));
    if (nv24At >= 0) {
        Check(withList[nv24At].optimality ==
                  VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
              "and it is OPTIMAL, read off the device rather than the list",
              "optimality " + U32((uint32_t)withList[nv24At].optimality));
        Check(withList[nv24At].encodeFormat == nv24,
              "naming itself, because nothing converted it",
              "names " + U32((uint32_t)withList[nv24At].encodeFormat));
    }
    if (s410At >= 0) {
        Check(withList[s410At].optimality ==
                  VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL,
              "S410 likewise OPTIMAL",
              "optimality " + U32((uint32_t)withList[s410At].optimality));
    }
}

// WHY THIS EXISTS. Every SUBOPTIMAL entry is ENCODABLE_VIA_FILTER, and
// InitializeExt refuses exactly that class when the preprocess filter is not
// compiled in. Advertising them in such a build would hand a caller a format
// and then refuse the session declaring it -- the single failure this list is
// supposed to make impossible. The advertisement is gated on the same build
// condition, and this is what holds the two together.
//
// CALIBRATION, AND IT IS A TWO-BUILD ONE. This case is deliberately NOT
// compiled out with the filter: it runs in both configurations and branches
// on kFilterCompiledIn, so the configuration that matters -- the one without
// the filter -- is the one where it still asserts. A check that vanished
// alongside the feature would prove nothing about the build it was written
// for.
//
// The OPTIMAL expectation is stated as an absolute and is NOT conditioned on
// kFilterCompiledIn. That is the point of it: those entries are
// ENCODABLE_DIRECT and involve no filter, so the same four must appear in
// both builds. Conditioning it would let the gate quietly take the direct arm
// with it and still pass.
void CaseFilterlessBuildAdvertisesNoConvertedEntry()
{
    g_currentCase = "a build without the preprocess filter advertises no "
                    "converted entry, and the same direct ones";

    // All four DIRECT formats, so the OPTIMAL arm is fully exercised, and
    // NV12/P010 present so several converted entries resolve.
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                    // NV12
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,   // P010
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                    // NV24
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,   // S410
    };
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 4, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);

    uint32_t optimalCount = 0;
    uint32_t suboptimalCount = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
            optimalCount++;
        } else {
            suboptimalCount++;
        }
    }

    // UNCONDITIONAL. Four DIRECT formats on the device list, four OPTIMAL
    // entries, in both builds.
    Check(optimalCount == 4,
          "the direct arm advertises all four device formats, whether or not "
          "the filter is compiled in",
          "optimal " + U32(optimalCount));
    for (uint32_t j = 0; j < 4; j++) {
        bool present = false;
        for (uint32_t i = 0; i < n; i++) {
            present = present ||
                      ((out[i].format == deviceList[j]) &&
                       (out[i].optimality ==
                        VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL));
        }
        Check(present,
              Lbl(std::string(AdvName(deviceList[j])) +
                  ": advertised OPTIMAL in either build"),
              "device entry " + U32(j));
    }

    // CONDITIONAL, and this is the gate itself.
    if (kFilterCompiledIn) {
        Check(suboptimalCount == 7,
              "with the filter, the converted entries this device can reach "
              "are advertised -- the four 3-plane inputs whose semi-planar "
              "sibling is on this list, and the three RGBA spellings",
              "suboptimal " + U32(suboptimalCount));
    } else {
        Check(suboptimalCount == 0,
              "without the filter, no converted entry is advertised -- the "
              "session would refuse every one of them",
              "suboptimal " + U32(suboptimalCount));
    }

    // And the totals follow from the two above, stated so a drift in either
    // shows up as a count rather than only as a membership failure.
    Check(n == (kFilterCompiledIn ? 11u : 4u),
          "the advertised total is the direct set plus the converted set the "
          "build can actually honour",
          "total " + U32(n));
}
// The route is stated per entry, so a caller never has to infer it -- and the
// inference it would otherwise make is wrong on exactly the entry whose
// conversion target is its own format.
void CaseOptimalityNamesTheEncodersOwnFormat()
{
    g_currentCase = "optimality names the encoder-read set, on "
                    "every entry of every list";
    const VkFormat deviceList[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
    };
    VkVideoEncoderInputFormatProperties out[VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t n = AdvertiseFromDeviceList(
        deviceList, 4, out, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    Check(n > 0, "the list is non-empty, so the loop below measures something",
          "got " + U32(n));
    uint32_t equalButFiltered = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool inDeviceList = false;
        for (uint32_t j = 0; j < 4; j++) {
            inDeviceList = inDeviceList || (deviceList[j] == out[i].format);
        }
        const bool routedUnconverted =
            inDeviceList &&
            (VkEncClassifyInput(out[i].format,
                                VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
             VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT);
        Check((out[i].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) ==
                  routedUnconverted,
              Lbl(std::string(AdvName(out[i].format)) +
                  ": optimality is OPTIMAL exactly where the device takes it "
                  "unconverted"),
              "entry " + U32(i) + " optimality " +
                  U32((uint32_t)out[i].optimality));
        // An OPTIMAL entry always names itself; a SUBOPTIMAL one may too.
        if (out[i].optimality == VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) {
            Check(out[i].format == out[i].encodeFormat,
                  Lbl(std::string(AdvName(out[i].format)) +
                      ": a direct entry names itself"),
                  "names " + U32((uint32_t)out[i].encodeFormat));
        } else if (out[i].format == out[i].encodeFormat) {
            equalButFiltered++;
        }
        // Every advertised target must be something the device takes,
        // whichever route the entry takes to it.
        bool targetInDeviceList = false;
        for (uint32_t j = 0; j < 4; j++) {
            targetInDeviceList =
                targetInDeviceList || (deviceList[j] == out[i].encodeFormat);
        }
        Check(targetInDeviceList,
              Lbl(std::string(AdvName(out[i].format)) +
                  ": what it is encoded as is a format the device accepts"),
              "target " + U32((uint32_t)out[i].encodeFormat));
    }
    // THE COLLISION, DRIVEN. P012 is on this device list and is advertised
    // with encodeFormat == format, so a caller reading the equality alone
    // would call it direct. Exactly one entry is in that position, and it is
    // FILTERED.
    // The collision needs a converted entry to exist, so it is a claim about
    // a build that has the filter. Without one there is no filtered entry at
    // all, which the count below states rather than skips.
    Check(equalButFiltered == (kFilterCompiledIn ? 1u : 0u),
          "one advertised entry names its own format and is still filtered, "
          "which is the case the equality cannot answer -- and none at all "
          "where no converted entry is advertised",
          "found " + U32(equalButFiltered));
}

// The conversion target is DERIVED, and this is the derivation stated as a
// property rather than as a table: the filter's Y'CbCr arm changes plane
// layout and packing and resamples neither chroma nor bit depth, so the
// target must agree with its input on both and must be semi-planar.
void CaseConversionTargetPreservesSubsamplingAndDepth()
{
    g_currentCase = "a Y'CbCr conversion target keeps the subsampling and "
                    "the bit depth of its input";
    uint32_t routableCount = 0;
    const VkFormat* routable = VkEncRoutableInputFormats(routableCount);
    uint32_t checked = 0;
    for (uint32_t i = 0; i < routableCount; i++) {
        const VkFormat in = routable[i];
        const VkMpFormatInfo* inInfo = YcbcrVkFormatInfo(in);
        if (inInfo == nullptr) {
            continue;   // the RGB half; its target is the device's choice
        }
        // Asked with a device list that carries every semi-planar target, so
        // the derivation is exercised rather than the membership rule.
        const VkFormat targets[] = {
            VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
            VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
            VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
            VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
        };
        const VkFormat target = VkEncConversionTargetFormat(in, targets, 5);
        if (target == VK_FORMAT_UNDEFINED) {
            // Direct inputs convert into nothing, which is the honest answer.
            Check(VkEncClassifyInput(
                      in, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                      VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
                  Lbl(std::string(AdvName(in)) +
                      ": only a directly encodable input has no target"),
                  "no target for a converted input");
            continue;
        }
        const VkMpFormatInfo* outInfo = YcbcrVkFormatInfo(target);
        Check(outInfo != nullptr,
              Lbl(std::string(AdvName(in)) + ": its target is a Y'CbCr format"),
              "target " + U32((uint32_t)target));
        if (outInfo == nullptr) {
            continue;
        }
        Check(outInfo->planesLayout.bpp == inInfo->planesLayout.bpp,
              Lbl(std::string(AdvName(in)) + ": the target keeps the bit depth"),
              "in " + U32(inInfo->planesLayout.bpp) + ", out " +
                  U32(outInfo->planesLayout.bpp));
        Check((outInfo->planesLayout.secondaryPlaneSubsampledX ==
               inInfo->planesLayout.secondaryPlaneSubsampledX) &&
                  (outInfo->planesLayout.secondaryPlaneSubsampledY ==
                   inInfo->planesLayout.secondaryPlaneSubsampledY),
              Lbl(std::string(AdvName(in)) + ": the target keeps the subsampling"),
              "");
        Check(outInfo->planesLayout.numberOfExtraPlanes == 1u,
              Lbl(std::string(AdvName(in)) + ": the target is semi-planar"),
              "extra planes " +
                  U32(outInfo->planesLayout.numberOfExtraPlanes));
        checked++;
    }
    // Twelve: the 3-plane family at three subsamplings and three depths,
    // plus the three semi-planar 12-bit rows whose target is themselves. The
    // number is stated so that a derivation that quietly stopped naming
    // targets shows up as a count rather than only as a silent pass over an
    // empty loop.
    Check(checked == 12,
          "twelve Y'CbCr inputs have a conversion target",
          "checked " + U32(checked));
}

// The routable list and the classifier are two statements of one set, and a
// change to either that does not change the other is what this catches.
void CaseRoutableListAgreesWithTheClassifier()
{
    g_currentCase = "the routable list and the classifier name the same set";
    uint32_t routableCount = 0;
    const VkFormat* routable = VkEncRoutableInputFormats(routableCount);
    for (uint32_t i = 0; i < routableCount; i++) {
        Check(VkEncClassifyInput(routable[i],
                                 VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
                  VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              Lbl(std::string(AdvName(routable[i])) +
                  " is on the routable list and the classifier routes it"),
              "the classifier says UNSUPPORTED");
    }
    // The other direction, over the formats this suite reaches: nothing the
    // classifier routes may be missing from the list, or the advertisement
    // would silently never offer it. Population: 22 candidates. This is the
    // SPOT check; CaseRoutableSetIsDerivedFromTheFormatTables walks the whole
    // table in both directions and is what actually holds the predicate.
    const VkFormat candidates[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
        VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        // The neighbours. Some are refused for a stated reason elsewhere in
        // this file -- the two sRGB spellings, the two packed 4:4:4 aliases,
        // scRGB, packed 4:2:2, and the 16-bit rows -- and some are ROUTED,
        // which is the half of this loop that matters: a routed format that
        // is off the list fails here. Semi-planar and 3-plane 4:2:2 and
        // 3-plane 4:4:4 are in the second group since the set was derived.
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_G8B8G8R8_422_UNORM,
        VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,
        VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM,
        VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM,
        VK_FORMAT_G16_B16R16_2PLANE_420_UNORM,
        VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM,
    };
    uint32_t routedOffList = 0;
    for (VkFormat f : candidates) {
        if (VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
            VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
            continue;
        }
        bool onList = false;
        for (uint32_t i = 0; i < routableCount; i++) {
            onList = onList || (routable[i] == f);
        }
        if (!onList) {
            routedOffList++;
            Check(false,
                  "a format the classifier routes is on the routable list",
                  "format " + U32((uint32_t)f) + " is routed but not listed");
        }
    }
    Check(routedOffList == 0,
          "0 of 22 candidate formats are routed without being on the list",
          U32(routedOffList) + " were");
}

// THE DERIVATION, PINNED. The routable set is COMPUTED from the multi-planar
// Y'CbCr format table rather than listed, so what a test can hold it to is the
// PREDICATE. Asserting a copy of the answer would only move the literal this
// replaced into the test file.
//
// Walked over the whole table in BOTH directions -- every row the predicate
// admits is on the list, every row it refuses is off it -- which is what makes
// this a test of the rule rather than a spot check of six formats.
//
// The predicate is restated here from the two table fields it reads, and
// deliberately NOT by calling the library's own helpers: a test that asked the
// implementation what the implementation does would agree with any answer.
void CaseRoutableSetIsDerivedFromTheFormatTables()
{
    g_currentCase = "the routable set is the format table filtered by the "
                    "predicate, in both directions";
    uint32_t routableCount = 0;
    const VkFormat* routable = VkEncRoutableInputFormats(routableCount);

    uint32_t rows = 0;
    uint32_t admitted = 0;
    uint32_t refused = 0;
    for (uint32_t i = 0;; i++) {
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfoByIndex(i);
        if (mpInfo == nullptr) {
            break;
        }
        rows++;
        const uint32_t layout = mpInfo->planesLayout.layout;
        const uint32_t bits   = GetBitsPerChannel(mpInfo->planesLayout);
        const bool modelledLayout =
            (layout == YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED) ||
            (layout == YCBCR_PLANAR_STRIDE_PADDED);
        const bool encodableDepth =
            (bits == 8u) || (bits == 10u) || (bits == 12u);
        const bool direct = (layout == YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED) &&
                            ((bits == 8u) || (bits == 10u));
        // A converted row is routed only where the table names a semi-planar
        // sibling at its own depth and subsampling to convert into. Asked of
        // the table here too, so the target rule is pinned alongside the
        // membership rule rather than assumed.
        bool hasSibling = false;
        for (uint32_t j = 0;; j++) {
            const VkMpFormatInfo* other = YcbcrVkFormatInfoByIndex(j);
            if (other == nullptr) {
                break;
            }
            hasSibling = hasSibling ||
                ((other->planesLayout.layout ==
                  YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED) &&
                 (GetBitsPerChannel(other->planesLayout) == bits) &&
                 (other->planesLayout.secondaryPlaneSubsampledX ==
                  mpInfo->planesLayout.secondaryPlaneSubsampledX) &&
                 (other->planesLayout.secondaryPlaneSubsampledY ==
                  mpInfo->planesLayout.secondaryPlaneSubsampledY));
        }
        const bool expectRouted =
            modelledLayout && encodableDepth && (direct || hasSibling);

        bool onList = false;
        for (uint32_t k = 0; k < routableCount; k++) {
            onList = onList || (routable[k] == mpInfo->vkFormat);
        }
        Check(onList == expectRouted,
              Lbl("table row " + U32(i) + " (format " +
                  U32((uint32_t)mpInfo->vkFormat) + ", " +
                  AdvName(mpInfo->vkFormat) +
                  ") is on the routable list exactly when the predicate "
                  "admits it"),
              std::string(onList ? "listed" : "absent") + ", predicate says " +
                  (expectRouted ? "route" : "refuse"));
        // And the classifier has to agree with the list, since both are the
        // same derivation read twice.
        Check((VkEncClassifyInput(mpInfo->vkFormat,
                                  VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
               VK_ENC_INPUT_FORMAT_UNSUPPORTED) == expectRouted,
              Lbl(std::string("row ") + U32(i) +
                  ": the classifier answers the same predicate"),
              "class " + U32((uint32_t)VkEncClassifyInput(
                  mpInfo->vkFormat, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT)));
        if (expectRouted) {
            admitted++;
        } else {
            refused++;
        }
    }

    // CALIBRATION. The walk has to see BOTH answers, or the loop above would
    // be asserting one of them over an empty population and would pass for a
    // derivation that routed everything, or nothing.
    Check(rows == (uint32_t)YCBCR_VK_FORMAT_INFO_TABLE_SIZE,
          "the walk visits every row of the multi-planar table",
          U32(rows) + " rows");
    Check(admitted > 0, "the predicate admits some rows", U32(admitted));
    Check(refused > 0, "and refuses others", U32(refused));

    // The RGB spellings are the rest of the list, and they are not in this
    // table at all: an RGB layout is precisely what it does not describe.
    Check(routableCount == (admitted + 3u),
          "the list is the admitted table rows plus the three RGB spellings",
          U32(routableCount) + " listed, " + U32(admitted) + " admitted");

    for (uint32_t i = 0; i < routableCount; i++) {
        uint32_t seen = 0;
        for (uint32_t j = 0; j < routableCount; j++) {
            if (routable[j] == routable[i]) {
                seen++;
            }
        }
        Check(seen == 1, "each routable format appears exactly once",
              "format " + U32((uint32_t)routable[i]) + " appears " +
                  U32(seen));
    }
}

// THE PACKED 4:2:2 FAMILY IS REFUSED, AND ON ITS LAYOUT. Singled out because
// it is the one exclusion that is about the shader generator being WRONG
// rather than about a Vulkan or codec limit: those rows are ONE plane and the
// table gives them numberOfExtraPlanes = 1, so the generator declares a
// two-plane read over a single-plane image and its luma addressing assumes one
// sample per texel on a format carrying two. The shader compiles either way,
// so nothing downstream would catch it -- a widening that dropped the layout
// predicate would admit them silently and produce a plausible wrong picture.
void CaseSinglePlaneInterleavedIsRefused()
{
    g_currentCase = "the packed 4:2:2 family is refused, at every depth";
    const VkFormat packed422[] = {
        VK_FORMAT_G8B8G8R8_422_UNORM,                       // YUY2
        VK_FORMAT_B8G8R8G8_422_UNORM,                       // UYVY
        VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16,   // Y210
        VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16,   // Y212
        VK_FORMAT_G16B16G16R16_422_UNORM,                   // Y216
    };
    for (VkFormat f : packed422) {
        // The row EXISTS in the table, so the refusal is the predicate's and
        // not a lookup miss. That distinction is the whole point: a format the
        // table does not describe is refused by accident.
        Check(YcbcrVkFormatInfo(f) != nullptr,
              "the format table describes it, so the refusal is a decision",
              "format " + U32((uint32_t)f));
        Check(VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                  VK_ENC_INPUT_FORMAT_UNSUPPORTED,
              "classified UNSUPPORTED", "format " + U32((uint32_t)f));
        Check(VkEncConversionTargetFormat(f, nullptr, 0) ==
                  VK_FORMAT_UNDEFINED,
              "and names no conversion target", "format " +
                  U32((uint32_t)f));
    }
    // CALIBRATION, and it is the one that matters: the SEMI-PLANAR row at the
    // same 4:2:2 subsampling IS routed. Without it this case would pass just
    // as well for a library that refused 4:2:2 outright, which is a different
    // and weaker claim.
    Check(VkEncClassifyInput(VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,
                             VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) !=
              VK_ENC_INPUT_FORMAT_UNSUPPORTED,
          "semi-planar 4:2:2 at the same subsampling IS routed, so the "
          "refusals above are the layout's and not the subsampling's",
          "NV16 was refused too");
}

// WHAT A 4:2:0 DEVICE SEES, AND THAT NEITHER DERIVING THE ROUTABLE SET NOR
// PARAMETERISING THE ADMISSION MOVED IT.
//
// The device lists below are 4:2:0 and are supplied by this file, not read off
// a driver: what they record is the advertised answer as it stood before the
// routable set was derived -- not re-derived here, which would let the record
// and the code move together. They are still the right record after the
// admission became a callback, because a 4:2:0 candidate resolves at a 4:2:0
// profile and this synthetic admission answers exactly what the device-list
// form used to answer.
//
// The rows are the whole answer, in order, so a change that ADDED an entry
// fails as loudly as one that dropped it.
void CaseFourTwoZeroDeviceAdvertisesTheHistoricalSet()
{
    g_currentCase = "a 4:2:0 device list advertises exactly what it did "
                    "before the routable set was derived";
    struct Row { VkFormat in; VkFormat out; bool optimal; };
    struct Scenario {
        const char*   name;
        const VkFormat* device;
        uint32_t      deviceCount;
        const Row*    expected;
        uint32_t      expectedCount;
    };

    static const VkFormat kNv12[] = { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM };
    static const Row kNv12Expected[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  true  },
        { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_R8G8B8A8_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_A8B8G8R8_UNORM_PACK32,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
    };

    static const VkFormat k420Full[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
    };
    static const Row k420FullExpected[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  true  },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, true },
        { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_R8G8B8A8_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
        { VK_FORMAT_A8B8G8R8_UNORM_PACK32,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,  false },
    };

    // P010 alone, which is not a hypothetical: it is what an NVIDIA RTX A4000
    // (0x10DE:0x24B0, driver 620.72.0) reports as the encode source for
    // H.265 Main 10 at 4:2:0 / 10 bits, measured with
    // vkGetPhysicalDeviceVideoFormatPropertiesKHR. Every RGB entry names P010
    // here because an RGB session takes the device's FIRST choice and that is
    // the only choice.
    static const VkFormat kP010Only[] = {
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
    };
    static const Row kP010OnlyExpected[] = {
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, true  },
        { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_R8G8B8A8_UNORM,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_B8G8R8A8_UNORM,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false },
        { VK_FORMAT_A8B8G8R8_UNORM_PACK32,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false },
    };

    const Scenario scenarios[] = {
        // NV12 alone is what the same A4000 reports for H.264 Baseline, Main
        // and High and for H.265 Main, all at 8 bits -- one format per
        // profile, which is also the only figure the corpus had from any
        // other device.
        { "NV12 only", kNv12, 1, kNv12Expected, 5 },
        { "P010 only (A4000, H.265 Main 10)", kP010Only, 1,
          kP010OnlyExpected, 5 },
        { "every 4:2:0 semi-planar depth", k420Full, 3, k420FullExpected, 9 },
    };

    for (const Scenario& sc : scenarios) {
        VkVideoEncoderInputFormatProperties out[
            VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
        const uint32_t n = AdvertiseFromDeviceList(
            sc.device, sc.deviceCount, out,
            VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
        // Without the filter the converted half is not advertised at all, so
        // the record is the OPTIMAL prefix of it. Stated rather than skipped,
        // because that build is the one this list's gate exists for.
        uint32_t wanted = 0;
        for (uint32_t i = 0; i < sc.expectedCount; i++) {
            if (kFilterCompiledIn || sc.expected[i].optimal) {
                wanted++;
            }
        }
        Check(n == wanted,
              Lbl(std::string(sc.name) + ": the advertised count is what it "
                  "was before the set was derived"),
              "got " + U32(n) + ", want " + U32(wanted));
        uint32_t at = 0;
        for (uint32_t i = 0; (i < sc.expectedCount) && (at < n); i++) {
            if (!kFilterCompiledIn && !sc.expected[i].optimal) {
                continue;
            }
            Check((out[at].format == sc.expected[i].in) &&
                      (out[at].encodeFormat == sc.expected[i].out) &&
                      ((out[at].optimality ==
                        VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL) ==
                       sc.expected[i].optimal),
                  Lbl(std::string(sc.name) + " entry " + U32(at) + ": " +
                      AdvName(sc.expected[i].in) + " -> " +
                      AdvName(sc.expected[i].out)),
                  "got " + U32((uint32_t)out[at].format) + " -> " +
                      U32((uint32_t)out[at].encodeFormat) + " opt " +
                      U32((uint32_t)out[at].optimality));
            at++;
        }
    }

    // CALIBRATION: the same function on a device list that DOES carry a 4:4:4
    // encode source answers differently, so the two records above are a
    // measurement and not a function that ignores its argument.
    const VkFormat with444[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
    };
    VkVideoEncoderInputFormatProperties wide[
        VK_ENC_MAX_ROUTABLE_INPUT_FORMATS] = {};
    const uint32_t wn = AdvertiseFromDeviceList(
        with444, 2, wide, VK_ENC_MAX_ROUTABLE_INPUT_FORMATS);
    Check(AdvIndexOf(wide, wn, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM) >=
              (kFilterCompiledIn ? 0 : -1) &&
              ((AdvIndexOf(wide, wn, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM) >= 0)
               == kFilterCompiledIn),
          "3-plane 4:4:4 IS advertised once the device reports NV24, and is "
          "absent from every 4:2:0 list above -- the device is what decides "
          "it, not this library",
          "index " + U32((uint32_t)(AdvIndexOf(
              wide, wn, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM) + 1)));
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

// THE TWO DERIVATIONS OF THE INPUT'S IDENTITY, AND THE ASSERTION THAT THEY
// AGREE.
//
// The binder derives (chroma subsampling, bit depth, plane count) FROM the
// caller's VkFormat. EncoderInputImageParameters::VerifyInputs() reconstructs a
// VkFormat FROM those same three. Two derivations of one quantity in opposite
// directions, and until now nothing said they had to agree -- the config that
// reached the encoder simply carried whatever the second one produced.
//
// THE REVERSE ONE CANNOT BE DELETED, which is why this is an assertion and not
// a removal. The packed-alias arm of the binder leaves input.vkFormat unwritten
// ON PURPOSE so the reconstruction supplies it: CodecGetVkFormat(4:4:4, depth,
// PACKED_1) spells AYUV at eight bits and Y410 at ten, and that is the only
// route by which either is nameable. Deleting the reverse derivation deletes
// two capabilities.
//
// WHAT THIS CASE READS. probe.inputVkFormat is the reconstruction's OUTPUT --
// input.vkFormat as it stands after InitializeParameters -- and the assertion
// is that it is the format the caller declared. Every routable candidate is
// swept, plus the two packed aliases, which the routable list cannot carry
// because it has no colour-model column.
//
// AND WHY THE PLANE COUNT IS STILL READ BESIDE IT. inputVkFormat is one value
// standing for three, so a disagreement says THAT the round trip broke and not
// WHICH term broke it. The triple is printed with every row for that reason.
//
// H.265 IS THE CODEC FOR EVERY ROW ON PURPOSE. Range Extensions is the one
// profile in this tree whose limits admit 4:2:0, 4:2:2 and 4:4:4 at 8, 10 and
// 12 bits together, so a refusal anywhere in this sweep is about the round trip
// and not about a profile that could not carry the input.
void CaseInputFormatSurvivesTheRoundTripThroughGeometry()
{
    g_currentCase = "the input format the binder derives geometry from is the "
                    "format that geometry reconstructs";

    struct Row {
        VkFormat                 format;
        VkVideoEncoderColorModel declared;
        const char*              what;
    };
    // The packed 4:4:4 aliases are named by hand: they ride RGBA enumerants and
    // are reached only through a Y'CbCr declaration, so the routable list --
    // which carries no colour model -- cannot name them.
    static const Row kPacked[] = {
        { VK_FORMAT_R8G8B8A8_UNORM, VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "AYUV (R8G8B8A8_UNORM declared Y'CbCr)" },
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          "Y410 (A2B10G10R10_UNORM_PACK32 declared Y'CbCr)" },
    };
    const uint32_t kPackedCount = (uint32_t)(sizeof(kPacked) / sizeof(kPacked[0]));

    uint32_t routableCount = 0;
    const VkFormat* const routable = VkEncRoutableInputFormats(routableCount);
    Check(routableCount >= 10u, "the routable set is worth sweeping",
          "count " + U32(routableCount));

    uint32_t swept = 0;
    for (uint32_t i = 0; i < routableCount + kPackedCount; i++) {
        const bool packed = (i >= routableCount);
        const VkFormat format =
            packed ? kPacked[i - routableCount].format : routable[i];
        const VkVideoEncoderColorModel declared =
            packed ? kPacked[i - routableCount].declared
                   : VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;
        const std::string what =
            packed ? std::string(kPacked[i - routableCount].what)
                   : ("routable enumerant " + U32((uint32_t)format));

        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        cfg.inputFormat     = format;
        cfg.inputColorModel = declared;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
        Check(r == VK_SUCCESS,
              ("the binder accepts it: " + what).c_str(),
              "VkResult " + U32((uint32_t)r));
        if (r != VK_SUCCESS) {
            continue;
        }
        swept++;
        Check(probe.inputVkFormat == (uint32_t)format,
              ("and the geometry it derived reconstructs the same format: " +
                  what).c_str(),
              "declared " + U32((uint32_t)format) + ", reconstructed " +
                  U32(probe.inputVkFormat) + " from (subsampling " +
                  U32(probe.inputChromaSubsampling) + ", " +
                  U32(probe.inputBpp) + "-bit, " + U32(probe.inputNumPlanes) +
                  " planes)");
    }
    Check(swept == routableCount + kPackedCount,
          "every candidate bound, so no row was skipped into agreement",
          "bound " + U32(swept) + " of " +
              U32(routableCount + kPackedCount));
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
        // I420 is the one row here that needs converting, so a build without
        // the filter refuses it -- and refusing is correct, since the staging
        // copy cannot change plane count. The other two are read directly and
        // are accepted in either build. This row asserted acceptance
        // unconditionally and so could only ever have been run with the
        // filter present.
        const bool needsFilter =
            (VkEncClassifyInput(f, VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
             VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER);
        if (needsFilter && !kFilterCompiledIn) {
            Check(r == VK_ERROR_INITIALIZATION_FAILED,
                  "a converted 4:2:0 input is refused, not bound, when the "
                  "build has no filter to convert it",
                  "format " + U32((uint32_t)f) + " VkResult " +
                      U32((uint32_t)r));
            continue;
        }
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

void CasePackedYcbcrIsRoutedWhereItIsDeclared()
{
    g_currentCase = "the packed 4:4:4 aliases are routed where they are declared";
    // WHAT THE COLOUR-MODEL DECLARATION BUYS. AYUV and Y410 are Y'CbCr 4:4:4
    // carried one interleaved texel per pixel. They have no Vulkan enumerant
    // of their own and ride R8G8B8A8_UNORM and A2B10G10R10_UNORM_PACK32,
    // which are also how an ordinary R'G'B' frame is spelled, so the
    // declaration is the only thing that can say which of the two a surface
    // is -- and the taxonomy has to ROUTE what the declaration names, or
    // stating the truth about the pixels is what refuses them.
    struct Row {
        const char*              name;
        VkFormat                 format;
        VkVideoEncoderColorModel declared;
        VkEncInputFormatClass    wantClass;
        uint32_t                 wantPlanes;
        uint32_t                 wantBpp;      // 0 when the class is UNSUPPORTED
        const char*              why;
    };
    static const Row rows[] = {
        { "AYUV", VK_FORMAT_R8G8B8A8_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER, 1, 8,
          "AYUV declared Y'CbCr is routed through the filter" },
        { "Y410", VK_FORMAT_A2B10G10R10_UNORM_PACK32,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER, 1, 10,
          "Y410 declared Y'CbCr is routed through the filter" },
        // Y416 is the one the packed table names and the taxonomy does not
        // take. 16 bits per component is not a
        // VkVideoComponentBitDepthFlagBitsKHR, so no input geometry can carry
        // it; the refusal is early and clear rather than an opaque one raised
        // after the caller has built a frame pool.
        { "Y416", VK_FORMAT_R16G16B16A16_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR,
          VK_ENC_INPUT_FORMAT_UNSUPPORTED, 0, 0,
          "Y416 declared Y'CbCr is refused for its bit depth" },
        // THE CONTROL, and it is the whole point of the case: the SAME
        // enumerant as the first row, declaring nothing. It must still be an
        // ordinary 8-bit R'G'B' image bound at 4:2:0, so a change that made
        // the packed rows pass by widening the R'G'B' arm would fail here.
        { "RGBA8", VK_FORMAT_R8G8B8A8_UNORM,
          VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT,
          VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER, 1, 8,
          "the same enumerant undeclared is still R'G'B'" },
    };
    for (const Row& row : rows) {
        Check(VkEncClassifyInput(row.format, row.declared) == row.wantClass,
              row.why,
              std::string(row.name) + " classified " +
                  U32((uint32_t)VkEncClassifyInput(row.format, row.declared)));
        Check(VkEncInputFormatPlaneCount(row.format) == row.wantPlanes,
              "and its layout is one plane, or none if it is not routed",
              std::string(row.name) + " planes " +
                  U32(VkEncInputFormatPlaneCount(row.format)));
    }

    // WHAT THE BINDER THEN WRITES. A class answer that no session geometry
    // follows would be an acceptance in name only: EncoderConfig does not
    // store the input format, it RECONSTRUCTS it from subsampling, bit depth
    // and plane count, so a packed input left at the 3-plane 4:2:0 default
    // would configure the session as I420 while the caller declared AYUV.
    // The probe reads exactly those three values back.
    if (!kFilterCompiledIn) {
        Check(true, "no preprocess filter is compiled into this build", "");
        return;
    }
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat     = row.format;
        cfg.inputColorModel = row.declared;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        if (row.wantClass == VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
            Check(r != VK_SUCCESS, "the binder refuses it too",
                  std::string(row.name) + " VkResult " + U32((uint32_t)r));
            continue;
        }
        Check(r == VK_SUCCESS, "the binder accepts it",
              std::string(row.name) + " VkResult " + U32((uint32_t)r));
        Check(probe.preprocessComputeFilter == 1,
              "and builds the filter without being asked",
              std::string(row.name) + " got " +
                  U32(probe.preprocessComputeFilter));
        Check(probe.inputNumPlanes == row.wantPlanes,
              "and describes the input as single-plane",
              std::string(row.name) + " got " + U32(probe.inputNumPlanes));
        Check(probe.inputBpp == row.wantBpp,
              "and at the component depth the layout carries",
              std::string(row.name) + " got " + U32(probe.inputBpp));
        // The subsampling is where the control separates from the packed
        // rows: a packed 4:4:4 input must move it off the 4:2:0 default,
        // and the same enumerant read as R'G'B' must not -- an R'G'B'
        // session's subsampling is the encode profile's, not the input's.
        const uint32_t wantSubsampling =
            (row.declared == VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR)
                ? (uint32_t)VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR
                : (uint32_t)VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        Check(probe.inputChromaSubsampling == wantSubsampling,
              "and at the subsampling the declaration implies",
              std::string(row.name) + " got " +
                  U32(probe.inputChromaSubsampling) + ", want " +
                  U32(wantSubsampling));
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
    //
    // THE UNBINDABLE ROWS NAME NUMBERS THE STANDARD'S LIMITS TABLE DOES NOT
    // STATE, so they must not name 122, H.265 4 or AV1 1 -- the library binds
    // those. Worse, AV1 High over this case's 4:2:0 input is refused by the
    // SUBSAMPLING guard rather than by unbindability, so such a row keeps
    // passing while asserting something that has stopped
    // being true. A refusal row is only evidence when nothing else produces the
    // same code.
    struct Row { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                 const char* why; };
    static const Row rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          "H.265 Main (1) on an H.264 session" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          "H.264 High (100) on an H.265 session" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 44,
          "H.264 CAVLC 4:4:4 Intra (44), an assigned profile_idc the limits "
          "table does not state and this library does not bind" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, 5,
          "H.265 High Throughput (5), likewise unstated and unbound" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, 3,
          "AV1 seq_profile 3, which the format does not define at all" },
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


void CaseProfileMustAdmitTheInputSubsampling()
{
    g_currentCase = "a profile the input subsampling does not admit is refused";
    // THE OTHER HALF OF THE STANDARD'S RULE. A guard reading the input bit depth
    // and nothing else lets an
    // explicitly named H.264 High (100) over 4:4:4 input passed the 8-bit
    // check, bound profile_idc 100, and OVERRODE the derivation that reads
    // input.chromaSubsampling and would have chosen High 4:4:4 Predictive
    // (244). The result declared 4:2:0 in the SPS while the session carried
    // 4:4:4 content -- a bitstream describing something the caller never
    // asked for, which is the exact failure the profile guard exists to
    // prevent on the depth axis.
    //
    // These rows are DEVICE-FREE: VkEncBuildAndProbeConfig binds a config and
    // reads it back without an encode-capable device, so what they measure is
    // the library's rule and not a driver's answer. The device half of the
    // same fact is measured separately, where a device exists.
    struct Row { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                 VkFormat fmt; const char* why; };
    static const Row rows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          "H.264 High (100) over 4:4:4 (NV24) input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_MAIN,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          "H.264 Main (77) over 4:4:4 (NV24) input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_BASELINE,
          VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,
          "H.264 Baseline (66) over 4:2:2 input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          "H.265 Main (1) over 4:4:4 (NV24) input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN10,
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,
          "H.265 Main 10 (2) over 10-bit 4:4:4 (S410) input" },
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

    // CONTROL 1 -- THE SAME PROFILE AT ITS OWN SUBSAMPLING IS STILL ACCEPTED.
    // A guard that refused everything would pass every row above. These say
    // the rows measure the profile/subsampling PAIR and not the presence of an
    // explicit profile, and not the format.
    struct OkRow { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                   VkFormat fmt; uint32_t expectBound; const char* what; };
    static const OkRow okRows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
          "H.264 High (100) over 4:2:0 (NV12) input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_BASELINE,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_PROFILE_H264_BASELINE,
          "H.264 Baseline (66) over 4:2:0 (NV12) input" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
          "H.265 Main (1) over 4:2:0 (NV12) input" },
    };
    for (const OkRow& row : okRows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec       = row.codec;
        cfg.profile     = row.profile;
        cfg.inputFormat = row.fmt;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        Check(r == VK_SUCCESS,
              (std::string("accepted: ") + row.what).c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(probe.codecProfile == row.expectBound,
              (std::string("and binds it unchanged: ") + row.what).c_str(),
              "got " + U32(probe.codecProfile));
    }

    // CONTROL 2 -- AND THE DERIVATION THE REFUSAL POINTS AT ACTUALLY EXISTS.
    // Every refusal above tells the caller to use DEFAULT instead. That advice
    // is only worth giving if DEFAULT reaches a profile that CAN carry the
    // input, so the same 4:4:4 formats are put through DEFAULT here and the
    // derived number is read back. Without this the refusals would be a dead
    // end dressed as a remedy.
    struct DerRow { VkVideoCodecOperationFlagBitsKHR codec; VkFormat fmt;
                    uint32_t expect; const char* what; };
    static const DerRow derRows[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE,
          "H.264 DEFAULT over 4:4:4 derives High 4:4:4 Predictive (244)" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,
          STD_VIDEO_H264_PROFILE_IDC_HIGH_422,
          "H.264 DEFAULT over 4:2:2 derives High 4:2:2 (122)" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS,
          "H.265 DEFAULT over 4:4:4 derives Range Extensions (4)" },
    };
    for (const DerRow& row : derRows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec       = row.codec;
        cfg.profile     = VK_VIDEO_ENCODER_PROFILE_DEFAULT;
        cfg.inputFormat = row.fmt;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(cfg, row.codec, &probe);
        Check(r == VK_SUCCESS,
              (std::string("accepted: ") + row.what).c_str(),
              "VkResult " + U32((uint32_t)r));
        Check(probe.codecProfile == row.expect,
              row.what, "got " + U32(probe.codecProfile));
    }
}

//=============================================================================
// VkVideoEncoderInputColourInfo -- what the caller's OWN samples carry.
//
// The config's colour fields say what the BITSTREAM should advertise. This
// chain says what the INPUT is. Before it, the RGBA->Y'CbCr filter derived its
// matrix from colourPrimaries -- an output field -- which is sound only because
// this library performs no primaries conversion, and nothing said so.
//
// Every case here is device-free: VkEncBuildAndProbeConfig walks pNext and the
// derivation runs inside the binder.
//=============================================================================

VkVideoEncoderInputColourInfo InputColour(uint8_t primaries, uint8_t transfer,
                                          uint8_t matrix,
                                          VkVideoEncoderRangeDeclaration range)
{
    VkVideoEncoderInputColourInfo ic{};
    ic.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO;
    ic.pNext = nullptr;
    ic.inputColourPrimaries         = primaries;
    ic.inputTransferCharacteristics = transfer;
    ic.inputMatrixCoefficients      = matrix;
    ic.reserved                     = 0;
    ic.inputRange                   = range;
    return ic;
}

void CaseInputColourChainBindsEachAxis()
{
    g_currentCase = "the chained input-colour struct reaches the encoder "
                    "config on every axis";
    // An RGBA session, because that is the lane the declaration steers, and
    // BT.2020 on both sides so the axes AGREE and the refusal below is not
    // what is being measured here.
    VkVideoEncoderInputColourInfo ic =
        InputColour(9, 14, 9, VK_VIDEO_ENCODER_RANGE_FULL);
    VkVideoEncoderConfig cfg = BaseConfig();
    cfg.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    cfg.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    cfg.colourPrimaries         = 9;
    cfg.transferCharacteristics = 14;
    cfg.matrixCoefficients      = 9;
    cfg.pNext = &ic;
    VkEncBoundConfigProbe probe{};
    const VkResult r = VkEncBuildAndProbeConfig(
        cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
    Check(r == VK_SUCCESS, "the binder accepted the chained input-colour "
                           "struct",
          "VkResult " + U32((uint32_t)r));
    Check(probe.inputColourChainPresent == 1u,
          "the chain is recorded as PRESENT, which no value field could say",
          "got " + U32(probe.inputColourChainPresent));
    Check(probe.inputColourPrimaries == 9u,
          "inputColourPrimaries bound", "got " + U32(probe.inputColourPrimaries));
    Check(probe.inputTransferCharacteristics == 14u,
          "inputTransferCharacteristics bound",
          "got " + U32(probe.inputTransferCharacteristics));
    Check(probe.inputMatrixCoefficients == 9u,
          "inputMatrixCoefficients bound",
          "got " + U32(probe.inputMatrixCoefficients));
    Check(probe.inputRange == (uint32_t)VK_VIDEO_ENCODER_RANGE_FULL,
          "inputRange bound", "got " + U32(probe.inputRange));
}

void CaseAbsentInputColourChainChangesNothing()
{
    g_currentCase = "an absent input-colour chain leaves every projected "
                    "field where it was";
    // THE DEVICE-FREE HALF OF THE REGRESSION CONTROL, and the one assertion
    // that protects every existing caller: a config with no chain must build
    // exactly what it built before. An ALL-ZERO chain is compared alongside
    // it, because "absent" and "present and all zero" must differ in exactly
    // one projected field -- the presence flag -- and in nothing else.
    VkVideoEncoderConfig plain = BaseConfig();
    plain.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    plain.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    plain.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    VkEncBoundConfigProbe noChain{};
    Check(VkEncBuildAndProbeConfig(
              plain, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &noChain) ==
              VK_SUCCESS,
          "an unchained RGBA config builds", "init failed");
    Check((noChain.inputColourChainPresent == 0u) &&
              (noChain.inputColourPrimaries == 0u) &&
              (noChain.inputTransferCharacteristics == 0u) &&
              (noChain.inputMatrixCoefficients == 0u) &&
              (noChain.inputRange == 0u),
          "an unchained config declares nothing about its input's colour",
          "present " + U32(noChain.inputColourChainPresent));
    // THE BT.709 FALLBACK SURVIVES for a genuinely undeclared input. This is
    // the regression the derivation change could have caused and did not.
    Check(noChain.matrixCoefficients == 0u,
          "and the binder writes no matrix for it, exactly as before",
          "got " + U32(noChain.matrixCoefficients));

    VkVideoEncoderInputColourInfo zero =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig chained = plain;
    chained.pNext = &zero;
    VkEncBoundConfigProbe zeroChain{};
    Check(VkEncBuildAndProbeConfig(
              chained, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
              &zeroChain) == VK_SUCCESS,
          "an all-zero chain is accepted", "init failed");
    Check(zeroChain.inputColourChainPresent == 1u,
          "an all-zero chain is PRESENT, which is the distinction no value "
          "field carries",
          "got " + U32(zeroChain.inputColourChainPresent));
    Check((zeroChain.inputColourPrimaries == 0u) &&
              (zeroChain.matrixCoefficients == noChain.matrixCoefficients) &&
              (zeroChain.colourPrimaries == noChain.colourPrimaries) &&
              (zeroChain.transferCharacteristics ==
               noChain.transferCharacteristics) &&
              (zeroChain.videoFullRangeFlag == noChain.videoFullRangeFlag) &&
              (zeroChain.colorDescriptionPresent ==
               noChain.colorDescriptionPresent) &&
              (zeroChain.videoSignalTypePresent ==
               noChain.videoSignalTypePresent),
          "and it changes nothing else -- undeclared is undeclared however it "
          "is spelled", "a projected colour field moved");
}

void CaseInputColourDisagreementIsRefused()
{
    g_currentCase = "an input colour that contradicts the bitstream's is "
                    "refused";
    // A MATCHED PAIR, AND THE CONTROL RUNS FIRST. There is no distinct error
    // code available -- every binder refusal is
    // VK_ERROR_INITIALIZATION_FAILED, and the sibling transfer-axis refusal
    // returns exactly that -- so an expectation on the code alone would also
    // match a refusal from somewhere else entirely and would assert nothing.
    // Two configs differing on ONE FIELD, one of which must succeed, is what
    // attributes the refusal to the axis under test.
    struct Pair {
        uint8_t     outputPrimaries;
        uint8_t     inputPrimaries;
        const char* what;
    };
    VkVideoEncoderConfig base = BaseConfig();
    base.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    base.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    base.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    base.colourPrimaries = 9;

    VkVideoEncoderInputColourInfo agree =
        InputColour(9, 0, 0, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig cfgA = base;
    cfgA.pNext = &agree;
    VkEncBoundConfigProbe probeA{};
    const VkResult rA = VkEncBuildAndProbeConfig(
        cfgA, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probeA);
    Check(rA == VK_SUCCESS,
          "CONTROL: input primaries 9 with bitstream primaries 9 is accepted",
          "VkResult " + U32((uint32_t)rA));
    if (rA != VK_SUCCESS) {
        // The pair is measuring a broken fixture; B's refusal would mean
        // nothing, so do not read it.
        return;
    }

    VkVideoEncoderInputColourInfo disagree =
        InputColour(1, 0, 0, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig cfgB = base;
    cfgB.pNext = &disagree;
    VkEncBoundConfigProbe probeB{};
    const VkResult rB = VkEncBuildAndProbeConfig(
        cfgB, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probeB);
    Check(rB == VK_ERROR_INITIALIZATION_FAILED,
          "and input primaries 1 with the SAME bitstream primaries 9 is "
          "refused -- one field apart, so the refusal is that field's",
          "VkResult " + U32((uint32_t)rB));

    // THE SAME SHAPE ON THE MATRIX AXIS, so "refused" is a property of the
    // rule and not of the primaries field alone.
    VkVideoEncoderConfig mBase = base;
    mBase.matrixCoefficients = 9;
    VkVideoEncoderInputColourInfo mAgree =
        InputColour(0, 0, 9, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig mA = mBase;
    mA.pNext = &mAgree;
    VkEncBoundConfigProbe mProbeA{};
    Check(VkEncBuildAndProbeConfig(
              mA, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &mProbeA) ==
              VK_SUCCESS,
          "CONTROL: input matrix 9 with bitstream matrix 9 is accepted",
          "init failed");
    VkVideoEncoderInputColourInfo mDisagree =
        InputColour(0, 0, 1, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig mB = mBase;
    mB.pNext = &mDisagree;
    VkEncBoundConfigProbe mProbeB{};
    Check(VkEncBuildAndProbeConfig(
              mB, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &mProbeB) ==
              VK_ERROR_INITIALIZATION_FAILED,
          "and input matrix 1 against bitstream matrix 9 is refused",
          "it was accepted");

    // AND A HALF-DECLARED PAIR IS NOT A DISAGREEMENT. 0 is UNDECLARED, so a
    // caller that states one side asserts nothing about the other. Without
    // this the refusal could be "any chain with a value in it".
    VkVideoEncoderInputColourInfo halfA =
        InputColour(1, 0, 0, VK_VIDEO_ENCODER_RANGE_UNDECLARED);
    VkVideoEncoderConfig half = BaseConfig();
    half.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    half.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    half.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    half.pNext           = &halfA;
    VkEncBoundConfigProbe halfProbe{};
    Check(VkEncBuildAndProbeConfig(
              half, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
              &halfProbe) == VK_SUCCESS,
          "an input primaries declaration against an UNDECLARED bitstream is "
          "accepted -- 0 asserts nothing to contradict",
          "it was refused");
}

// A DECLARED INPUT RANGE DECIDES THE STREAM'S RANGE ON THE LANE THAT APPLIES
// NOTHING, AND IS COMPARED ON THE LANE THAT APPLIES SOMETHING.
//
// EncoderConfig::video_full_range_flag is one variable with two consumers:
// the VUI bit the bitstream carries, and the VkSamplerYcbcrRange the
// preprocess filter is built with. They are one decision and the cases below
// read the projection of that one variable, so a change that moved only the
// VUI or only the filter would show up as a disagreement rather than as a
// pass.
//
// WHY THE TWO LANES DIFFER. On the Y'CbCr lane the library applies no range
// mapping -- the copy filter's own shader takes its output range from its
// input range, so a copy stays a copy -- and therefore the samples that
// arrive are the samples that are coded. The input's range IS the stream's
// range and a declaration of it is a fact about the stream. On the RGB lane
// the filter PRODUCES the Y'CbCr range, from this same flag, and the caller's
// declaration is about its RGB buffer; the two are different quantities, so
// the declaration is compared against what the filter can read and never
// silently retargets the output.
//
// THE REFUSAL ROWS COME IN PAIRS, one field apart. VK_ERROR_INITIALIZATION_
// FAILED is the binder's only refusal code, so a row that merely fails could
// be failing for any reason; each refusal below sits beside an otherwise
// identical config that succeeds, which is what makes the refusal that
// field's.
void CaseDeclaredInputRangeDecidesTheStreamsRange()
{
    g_currentCase = "a declared input range decides what the stream says";

    // CALIBRATION, BEFORE ANY DECLARATION IS READ. The two projected fields
    // this case turns on have to be shown to move at all, or a run in which
    // they were stuck at zero would read as "the declaration did nothing" and
    // as "the declaration is undeclared" identically.
    VkVideoEncoderConfig calFull = BaseConfig();
    calFull.videoFullRange = VK_TRUE;
    VkEncBoundConfigProbe calFullProbe{};
    Check(VkEncBuildAndProbeConfig(
              calFull, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &calFullProbe) == VK_SUCCESS,
          "calibration: videoFullRange = VK_TRUE builds", "init failed");
    Check((calFullProbe.videoFullRangeFlag == 1u) &&
              (calFullProbe.videoSignalTypePresent == 1u),
          "calibration: the two fields this case reads DO move -- "
          "videoFullRange raises both",
          "flag " + U32(calFullProbe.videoFullRangeFlag) + ", present " +
              U32(calFullProbe.videoSignalTypePresent));
    VkVideoEncoderConfig calBare = BaseConfig();
    VkEncBoundConfigProbe calBareProbe{};
    Check(VkEncBuildAndProbeConfig(
              calBare, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &calBareProbe) == VK_SUCCESS,
          "calibration: an undeclared config builds", "init failed");
    Check((calBareProbe.videoFullRangeFlag == 0u) &&
              (calBareProbe.videoSignalTypePresent == 0u),
          "calibration: and they are BOTH ZERO when nothing is declared, so "
          "the instrument reads two states and not one",
          "flag " + U32(calBareProbe.videoFullRangeFlag) + ", present " +
              U32(calBareProbe.videoSignalTypePresent));

    // ---- The Y'CbCr lane: the declaration is applied ----
    struct DirectRow {
        VkVideoEncoderRangeDeclaration   declared;
        uint32_t                         wantFlag;
        const char*                      what;
    };
    static const DirectRow directRows[] = {
        { VK_VIDEO_ENCODER_RANGE_FULL, 1u,
          "a Y'CbCr input declared FULL is coded as full range" },
        { VK_VIDEO_ENCODER_RANGE_LIMITED, 0u,
          "a Y'CbCr input declared LIMITED is coded as limited range" },
    };
    for (const DirectRow& row : directRows) {
        VkVideoEncoderInputColourInfo ic = InputColour(0, 0, 0, row.declared);
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.inputFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        cfg.pNext       = &ic;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, Lbl(std::string("accepted: ") + row.what),
              "VkResult " + U32((uint32_t)r));
        Check(probe.videoFullRangeFlag == row.wantFlag, Lbl(row.what),
              "video_full_range_flag " + U32(probe.videoFullRangeFlag) +
                  ", wanted " + U32(row.wantFlag));
        // AND IT IS SIGNALLED. An unsignalled range is inferred by the
        // standards and reported as unknown by decoders at their API
        // boundary, so a caller that declared one and got silence is no
        // better off than one that declared nothing.
        Check(probe.videoSignalTypePresent == 1u,
              Lbl(std::string("and it is SIGNALLED: ") + row.what),
              "video_signal_type_present_flag " +
                  U32(probe.videoSignalTypePresent));
    }

    // A DECLARED LIMITED IS NOT THE SAME AS SILENCE, and this is the pair
    // that says so: the same config without the chain signals nothing.
    VkVideoEncoderConfig unchained = BaseConfig();
    unchained.inputFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    VkEncBoundConfigProbe unchainedProbe{};
    Check(VkEncBuildAndProbeConfig(
              unchained, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &unchainedProbe) == VK_SUCCESS,
          "CONTROL: the same Y'CbCr config with no chain builds",
          "init failed");
    Check((unchainedProbe.videoSignalTypePresent == 0u) &&
              (unchainedProbe.videoFullRangeFlag == 0u),
          "CONTROL: and it declares nothing -- an absent chain is not a "
          "declaration of limited range",
          "present " + U32(unchainedProbe.videoSignalTypePresent) + ", flag " +
              U32(unchainedProbe.videoFullRangeFlag));

    // ---- The Y'CbCr lane: a contradiction is refused ----
    //
    // videoFullRange has no undeclared state -- it is a VkBool32 whose zero
    // is indistinguishable from silence -- so only the raised direction can
    // be contradicted, and only that direction is refused.
    VkVideoEncoderInputColourInfo limited =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_LIMITED);
    VkVideoEncoderConfig clash = BaseConfig();
    clash.inputFormat    = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    clash.videoFullRange = VK_TRUE;
    clash.pNext          = &limited;
    VkEncBoundConfigProbe clashProbe{};
    Check(VkEncBuildAndProbeConfig(
              clash, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &clashProbe) == VK_ERROR_INITIALIZATION_FAILED,
          "a LIMITED input under a full-range bitstream request is refused -- "
          "the library scales nothing",
          "it was accepted");

    VkVideoEncoderInputColourInfo full =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_FULL);
    VkVideoEncoderConfig agree = BaseConfig();
    agree.inputFormat    = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    agree.videoFullRange = VK_TRUE;
    agree.pNext          = &full;
    VkEncBoundConfigProbe agreeProbe{};
    Check(VkEncBuildAndProbeConfig(
              agree, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &agreeProbe) == VK_SUCCESS,
          "PAIR: the same config with the input declared FULL is accepted -- "
          "one field apart, so the refusal above is that field's",
          "it was refused");
    Check(agreeProbe.videoFullRangeFlag == 1u,
          "and both sides agreeing on full range still codes full range",
          "flag " + U32(agreeProbe.videoFullRangeFlag));

    // ---- The RGB lane: the declaration is compared, not applied ----
    VkVideoEncoderInputColourInfo rgbLimited =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_LIMITED);
    VkVideoEncoderConfig rgbBad = BaseConfig();
    rgbBad.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    rgbBad.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    rgbBad.pNext           = &rgbLimited;
    VkEncBoundConfigProbe rgbBadProbe{};
    Check(VkEncBuildAndProbeConfig(
              rgbBad, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &rgbBadProbe) == VK_ERROR_INITIALIZATION_FAILED,
          "an RGB input declared LIMITED is refused -- the filter reads its "
          "RGB over the full range and performs no input expansion",
          "it was accepted");

    VkVideoEncoderInputColourInfo rgbFull =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_FULL);
    VkVideoEncoderConfig rgbOk = BaseConfig();
    rgbOk.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    rgbOk.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    rgbOk.pNext           = &rgbFull;
    VkEncBoundConfigProbe rgbOkProbe{};
    Check(VkEncBuildAndProbeConfig(
              rgbOk, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &rgbOkProbe) == VK_SUCCESS,
          "PAIR: the same RGB config declared FULL is accepted -- one field "
          "apart",
          "it was refused");
    // AND IT DOES NOT RETARGET THE OUTPUT. The RGB buffer's range and the
    // Y'CbCr the filter emits are different quantities; the second is the
    // bitstream request's to state.
    VkVideoEncoderConfig rgbBare = BaseConfig();
    rgbBare.inputFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    rgbBare.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    VkEncBoundConfigProbe rgbBareProbe{};
    Check(VkEncBuildAndProbeConfig(
              rgbBare, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              &rgbBareProbe) == VK_SUCCESS,
          "CONTROL: the same RGB config with no chain builds", "init failed");
    Check((rgbOkProbe.videoFullRangeFlag ==
           rgbBareProbe.videoFullRangeFlag) &&
              (rgbOkProbe.videoSignalTypePresent ==
               rgbBareProbe.videoSignalTypePresent),
          "and a FULL declaration on the RGB lane leaves the bitstream's "
          "range where the caller put it",
          "flag " + U32(rgbOkProbe.videoFullRangeFlag) + " against " +
              U32(rgbBareProbe.videoFullRangeFlag));

    // ---- AV1, whose range syntax is unconditional ----
    //
    // color_range is a mandatory bit in every AV1 sequence header, so the
    // question there is never "is it signalled" but "which value" -- and
    // before a declaration existed the answer was always 0.
    VkVideoEncoderInputColourInfo av1Full =
        InputColour(0, 0, 0, VK_VIDEO_ENCODER_RANGE_FULL);
    VkVideoEncoderConfig av1 = BaseConfig();
    av1.codec       = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    av1.inputFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    av1.pNext       = &av1Full;
    VkEncBoundConfigProbe av1Probe{};
    Check(VkEncBuildAndProbeConfig(
              av1, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &av1Probe) ==
              VK_SUCCESS,
          "an AV1 session accepts a declared input range", "init failed");
    Check(av1Probe.videoFullRangeFlag == 1u,
          "and an AV1 input declared FULL writes color_range 1 rather than "
          "the unconditional 0 it wrote before",
          "flag " + U32(av1Probe.videoFullRangeFlag));
}

void CaseInputColourPrimariesDriveTheDerivedMatrix()
{
    g_currentCase = "the filter's matrix is derived from the INPUT's "
                    "primaries when they are declared";
    // matrixCoefficients 2 (Unspecified) is the arm that derives, and the
    // derived value is SIGNALLED back, so the probe reads what the filter will
    // actually apply. Declaring the primaries on the INPUT side and leaving
    // the bitstream's undeclared is the configuration that could only ever
    // have come out BT.709 before.
    struct Row {
        uint8_t     inputPrimaries;
        uint32_t    wantMatrix;
        const char* what;
    };
    static const Row rows[] = {
        { 9u, 9u, "input primaries 9 (BT.2020) derive matrix 9" },
        { 6u, 6u, "input primaries 6 (SMPTE 170M) derive matrix 6" },
        { 1u, 1u, "input primaries 1 (BT.709) derive matrix 1" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderInputColourInfo ic =
            InputColour(row.inputPrimaries, 0, 0,
                        VK_VIDEO_ENCODER_RANGE_UNDECLARED);
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec              = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        cfg.inputFormat        = VK_FORMAT_R8G8B8A8_UNORM;
        cfg.inputColorModel    = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
        cfg.matrixCoefficients = 2u;   // Unspecified: derive and signal
        cfg.pNext              = &ic;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, Lbl(std::string("bound: ") + row.what),
              "VkResult " + U32((uint32_t)r));
        if (r != VK_SUCCESS) {
            continue;
        }
        Check(probe.matrixCoefficients == row.wantMatrix, Lbl(row.what),
              "got " + U32(probe.matrixCoefficients) + ", want " +
                  U32(row.wantMatrix));
    }
    // THE CONTROL. With no chain the SAME config derives from the bitstream's
    // primaries, which is what it did before -- so the rows above measure
    // where the derivation READS and not that it derives at all.
    VkVideoEncoderConfig ctl = BaseConfig();
    ctl.codec              = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    ctl.inputFormat        = VK_FORMAT_R8G8B8A8_UNORM;
    ctl.inputColorModel    = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    ctl.matrixCoefficients = 2u;
    ctl.colourPrimaries    = 9u;
    VkEncBoundConfigProbe ctlProbe{};
    Check(VkEncBuildAndProbeConfig(
              ctl, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &ctlProbe) ==
              VK_SUCCESS,
          "CONTROL: the unchained config still builds", "init failed");
    Check(ctlProbe.matrixCoefficients == 9u,
          "and with no chain the derivation still reads the bitstream's "
          "primaries",
          "got " + U32(ctlProbe.matrixCoefficients));
}

// THE AV1 SEQUENCE HEADER'S SUBSAMPLING AGAINST THE seq_profile IT DERIVED.
//
// InitSequenceHeader hardcoded subsampling_x = subsampling_y = 1 under a
// comment calling 4:2:0 the only chroma format this encoder admits, while
// InitProfileLevel a hundred lines below picks seq_profile 1 from 4:4:4 input
// and 2 from 4:2:2. AV1 6.4.1 gives seq_profile 1 subsampling_x ==
// subsampling_y == 0, so the pair was an invalid sequence header: the two
// halves of one structure decided by two functions that disagreed.
//
// DEVICE-FREE IS THE ONLY PLACE THIS IS ASSERTABLE. AV1 High and Professional
// are absent from every driver this project can reach, so a 4:4:4 or 4:2:2 AV1
// session dies at the capability query before a sequence header is built.
// InitSequenceHeader needs no device, and the probe calls it and reads the
// colour config back -- the same mechanism the HDR payload projection uses.
// Passing this asserts that the header the library BUILDS is self-consistent.
// It asserts nothing about any driver accepting it.
//
// THE 4:2:0 ROWS ARE THE CONTROL AND THEY RUN FIRST: they read (1, 1) before
// this change and after it. A change that wrote (0, 0) unconditionally would
// satisfy every 4:4:4 row and fail these.
//
// (internal.h warns that STD_VIDEO_AV1_PROFILE_MAIN == 0, so a codecProfile of
// 0 on the AV1 arm is a real value rather than "arm not exercised". The 4:4:4
// and 4:2:2 rows read a non-zero profile, which removes that ambiguity where it
// would matter.)
// WHICH SIDE OF THE INPUT/ENCODE BOUNDARY EACH CODEC ARM READS.
//
// EncoderConfig carries the input's geometry and the ENCODE's geometry in
// separate fields, and they exist separately so that the encode value can
// differ from the input value -- a chroma resampler or a device-driven depth
// downgrade is what would make them differ. Today one writer sets the encode
// side from the input side and nothing else touches either, so the two are
// always equal and a read of the wrong one costs nothing.
//
// WHAT THE ARMS MUST READ, AND IT IS NOT A PREFERENCE. A codec profile is
// defined by the standard over the values CARRIED IN THE BITSTREAM: H.264
// Annex A Table A-1 constrains profile_idc against the SPS's chroma_format_idc
// and bit_depth_*_minus8; H.265 Annex A does the same for
// general_profile_idc; AV1 6.4.1 defines seq_profile over the sequence
// header's BitDepth, mono_chrome and subsampling_x/y. Every one of those
// syntax elements is written in this tree from the ENCODE fields. So a
// derivation that picks the profile from the INPUT side selects a profile for
// a picture that is not the one the syntax describes.
//
// WHAT THIS CASE ASSERTS is exactly that rule and nothing weaker: the profile
// the arm derived is the profile the ENCODE geometry implies, with the encode
// geometry read off the probe rather than assumed to equal the input's. The
// three tables below restate the standards' rule; they are a second statement
// of the derivation, deliberately, because a test that recomputed it by
// calling the derivation would agree with itself whichever side it read.
//
// IT IS SWEPT OVER EVERY ROUTABLE FORMAT AND ALL THREE CODECS, at
// VK_VIDEO_ENCODER_PROFILE_DEFAULT, which is the only profile value that
// reaches the derivations at all.
// IS THE ORACLE'S OWN ANSWER LEGAL AT THAT GEOMETRY?
//
// An expectation table can be wrong in a way no comparison against the code
// catches: if the code and the table make the same mistake the row passes and
// ratifies it. This is the second reading, from the standards' limits and not
// from the derivations -- H.264 Table A-1, H.265 A.3, AV1 6.4.1 and A.2 --
// and every row the sweep asserts is put through it first.
//
// IT IS NOT THE LIBRARY'S OWN LIMITS TABLE. VkEncGetProfileInputLimits states
// the same rule inside the library; calling it here would make the check
// agree with whatever that table says, which is exactly the shape of
// self-agreement this exists to break.
bool ProfileAdmitsGeometry(VkVideoCodecOperationFlagBitsKHR codec,
                           uint32_t profile, uint32_t subsampling,
                           uint32_t bpp)
{
    const bool is420 = (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
    const bool is422 = (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR);
    const bool is444 = (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR);
    switch ((uint32_t)codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            switch (profile) {
                case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
                case STD_VIDEO_H264_PROFILE_IDC_MAIN:
                case STD_VIDEO_H264_PROFILE_IDC_HIGH:
                    return is420 && (bpp == 8);
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_10:
                    return is420 && (bpp <= 10);
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_422:
                    return (is420 || is422) && (bpp <= 10);
                case STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE:
                    return (is420 || is422 || is444) && (bpp <= 14);
                default: return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            switch (profile) {
                case STD_VIDEO_H265_PROFILE_IDC_MAIN:
                case STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE:
                    return is420 && (bpp == 8);
                case STD_VIDEO_H265_PROFILE_IDC_MAIN_10:
                    return is420 && (bpp <= 10);
                case STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS:
                case STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS:
                    return (is420 || is422 || is444) && (bpp <= 16);
                default: return false;
            }
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            switch (profile) {
                case STD_VIDEO_AV1_PROFILE_MAIN:      return is420 && (bpp <= 10);
                case STD_VIDEO_AV1_PROFILE_HIGH:      return is444 && (bpp <= 10);
                case STD_VIDEO_AV1_PROFILE_PROFESSIONAL:
                    return (is420 || is422 || is444) && (bpp <= 12);
                default: return false;
            }
        default: return false;
    }
}

// "THE STANDARD ADMITS NO ANSWER THIS LIBRARY DERIVES." Returned instead of a
// profile for a geometry whose only in-spec H.264 answer the derivation does
// not produce; the sweep excludes those rows loudly rather than writing a
// forbidden profile into an expectation.
const uint32_t kNoAdmissibleProfile = 0xFFFFFFFFu;

uint32_t WantH264Profile(uint32_t subsampling, uint32_t bpp)
{
    // adaptiveTransformMode has no setter, so use8x8Transform is always true
    // and the Baseline/Main seeds are unreachable -- the derivation starts at
    // High and is only widened from there.
    if (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) {
        // 244 admits 8 to 14 bits at every chroma format, so this arm is
        // in-spec at every depth the library can reach.
        return STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
    }
    // TODO: H.264 above ten bits at 4:2:0 or 4:2:2 HAS NO IN-SPEC ANSWER FROM
    // THIS DERIVATION, and the defect is EncoderConfigH264::InitProfileLevel's,
    // not this table's. ITU-T H.264 Table A-1 admits High 10 (110) and High
    // 4:2:2 (122) to ten bits; the derivation selects them from any depth
    // above eight, so a twelve-bit 4:2:0 input derives 110 and a twelve-bit
    // 4:2:2 input derives 122, both out of spec. The in-spec answer at those
    // geometries is High 4:4:4 Predictive (244), which admits chroma formats
    // 0 to 3 and fourteen bits.
    //
    // Fixing the derivation is out of this change's scope -- no device here
    // encodes H.264 above eight bits, so the arm is unreachable in practice
    // and correcting it needs hardware nobody has -- but WRITING 110 INTO AN
    // ORACLE AS THE EXPECTED ANSWER IS NOT. That documents the defect as
    // correct, which is the one thing a test must not do. The rows are
    // excluded, counted and named instead, so the exclusion is visible on
    // every run rather than being a silently missing assertion.
    if (bpp > 10) {
        return kNoAdmissibleProfile;
    }
    if (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR) {
        return STD_VIDEO_H264_PROFILE_IDC_HIGH_422;
    }
    return (bpp > 8) ? (uint32_t)STD_VIDEO_H264_PROFILE_IDC_HIGH_10
                     : (uint32_t)STD_VIDEO_H264_PROFILE_IDC_HIGH;
}

uint32_t WantH265Profile(uint32_t subsampling, uint32_t bpp)
{
    if (subsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR) {
        return STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
    }
    if (bpp == 8) {
        return STD_VIDEO_H265_PROFILE_IDC_MAIN;
    }
    if (bpp <= 10) {
        return STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
    }
    return STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
}

uint32_t WantAv1Profile(uint32_t subsampling, uint32_t bpp)
{
    if ((bpp > 10) ||
        (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR)) {
        return STD_VIDEO_AV1_PROFILE_PROFESSIONAL;
    }
    if (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) {
        return STD_VIDEO_AV1_PROFILE_HIGH;
    }
    return STD_VIDEO_AV1_PROFILE_MAIN;
}

void CaseCodecArmsDeriveTheProfileFromTheEncodeGeometry()
{
    g_currentCase = "the profile each codec arm derives is the one the ENCODE "
                    "geometry implies";

    struct CodecRow {
        VkVideoCodecOperationFlagBitsKHR codec;
        const char*                      name;
        uint32_t (*want)(uint32_t, uint32_t);
    };
    // Indexed as well as iterated below, so the per-codec divergence counters
    // in the second pass line up with the arms they count.
    static const CodecRow kCodecs[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264",
          &WantH264Profile },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H.265",
          &WantH265Profile },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,  "AV1",
          &WantAv1Profile },
    };

    uint32_t routableCount = 0;
    const VkFormat* const routable = VkEncRoutableInputFormats(routableCount);
    uint32_t rows     = 0;
    uint32_t asserted = 0;
    uint32_t excluded = 0;
    for (const CodecRow& c : kCodecs) {
        for (uint32_t i = 0; i < routableCount; i++) {
            VkVideoEncoderConfig cfg = BaseConfig();
            cfg.codec       = c.codec;
            cfg.profile     = VK_VIDEO_ENCODER_PROFILE_DEFAULT;
            cfg.inputFormat = routable[i];
            VkEncBoundConfigProbe probe{};
            const VkResult r =
                VkEncBuildAndProbeConfig(cfg, c.codec, &probe);
            if (r != VK_SUCCESS) {
                continue;
            }
            rows++;
            const uint32_t want = c.want(probe.encodeChromaSubsampling,
                                         probe.encodeBitDepthLuma);
            if (want == kNoAdmissibleProfile) {
                excluded++;
                std::printf("  EXCLUDED [%s] %s enumerant %u: encode geometry "
                            "(subsampling %u, %u-bit) has no in-spec profile "
                            "this derivation produces; the arm derived %u. "
                            "See the TODO on WantH264Profile.\n",
                            g_currentCase, c.name, (uint32_t)routable[i],
                            probe.encodeChromaSubsampling,
                            probe.encodeBitDepthLuma, probe.codecProfile);
                continue;
            }
            asserted++;
            // THE EXPECTATION IS CHECKED BEFORE IT IS USED. A table that
            // names a profile the standard does not admit at that geometry is
            // documenting a defect as the right answer, whatever the code
            // then does.
            Check(ProfileAdmitsGeometry(c.codec, want,
                                        probe.encodeChromaSubsampling,
                                        probe.encodeBitDepthLuma),
                  (std::string(c.name) + " enumerant " +
                   U32((uint32_t)routable[i]) +
                   ": the profile this case EXPECTS is one the standard "
                   "admits at that geometry").c_str(),
                  "expects profile " + U32(want) + " at (subsampling " +
                      U32(probe.encodeChromaSubsampling) + ", " +
                      U32(probe.encodeBitDepthLuma) + "-bit)");
            Check(probe.codecProfile == want,
                  (std::string(c.name) + " enumerant " +
                   U32((uint32_t)routable[i]) +
                   ": the profile follows the ENCODE geometry").c_str(),
                  "encode geometry is (subsampling " +
                      U32(probe.encodeChromaSubsampling) + ", " +
                      U32(probe.encodeBitDepthLuma) +
                      "-bit) which implies profile " + U32(want) +
                      ", the arm derived " + U32(probe.codecProfile) +
                      "; the input side was (subsampling " +
                      U32(probe.inputChromaSubsampling) + ", " +
                      U32(probe.inputBpp) + "-bit)");
        }
    }
    Check(rows >= 40u,
          "the sweep bound enough rows across the three arms to be read",
          "bound " + U32(rows));
    // THE EXCLUSION CANNOT HOLLOW THE CASE OUT. Counted separately from the
    // bound rows so that a widening of the excluded geometry shows up here as
    // a falling assertion count rather than as a still-green run.
    Check(asserted >= 40u,
          "and enough of them carried an in-spec expectation to assert",
          "asserted " + U32(asserted) + " of " + U32(rows) + ", excluded " +
              U32(excluded));

    // ---- SECOND PASS: THE TWO SIDES MADE TO DIFFER ----
    //
    // WHAT THE PASS ABOVE CANNOT SAY. One writer sets the encode side from
    // the input side and nothing else touches either, so on every state the
    // pass above can reach the two are EQUAL -- and an assertion over equal
    // values is satisfied identically whichever side an arm reads. Revert all
    // three arms to input.bpp and every row above still passes, with the same
    // check count. It is a guard against a wrong derivation and no guard at
    // all against a wrong side, which is the regression it was written for.
    //
    // WHAT MAKES THE DIFFERENCE REACHABLE. EncoderConfig::InitializeParameters
    // derives the encode depth under a zero-means-unset guard whose own
    // comment calls an explicit encode depth "a request and not a default".
    // VkEncBuildAndProbeConfig's fourth argument states one, so the encode
    // side becomes the request and the input side stays the caller's format's.
    // The two now disagree, and the profile an arm derives says which it
    // read: nothing else in the configuration moved.
    //
    // THE REQUEST IS THE FAR END OF THE DEPTH RANGE from the input, because a
    // near one implies the same profile on most rows and a row where both
    // sides imply the same answer asserts nothing. How many rows actually
    // diverged is COUNTED PER CODEC and asserted non-zero below -- without
    // that this pass could go green having compared every row against itself.
    uint32_t divergent[3] = {0u, 0u, 0u};
    uint32_t requested    = 0u;
    for (uint32_t ci = 0; ci < 3u; ci++) {
        const CodecRow& c = kCodecs[ci];
        for (uint32_t i = 0; i < routableCount; i++) {
            VkVideoEncoderConfig cfg = BaseConfig();
            cfg.codec       = c.codec;
            cfg.profile     = VK_VIDEO_ENCODER_PROFILE_DEFAULT;
            cfg.inputFormat = routable[i];

            VkEncBoundConfigProbe derived{};
            if (VkEncBuildAndProbeConfig(cfg, c.codec, &derived) !=
                VK_SUCCESS) {
                continue;
            }
            const uint32_t request = (derived.inputBpp <= 10u) ? 12u : 8u;

            VkEncBoundConfigProbe stated{};
            if (VkEncBuildAndProbeConfig(cfg, c.codec, &stated, request) !=
                VK_SUCCESS) {
                continue;
            }
            // THE FIXTURE IS ASSERTED BEFORE THE PROPOSITION. If the request
            // did not land, or if it moved the input side too, the row below
            // would be measuring a broken instrument rather than an arm.
            const bool fixtureOk = (stated.encodeBitDepthLuma == request) &&
                                   (stated.inputBpp == derived.inputBpp) &&
                                   (stated.encodeChromaSubsampling ==
                                    derived.encodeChromaSubsampling);
            Check(fixtureOk,
                  Lbl(std::string(c.name) + " enumerant " +
                      U32((uint32_t)routable[i]) +
                      ": the stated encode depth landed on the ENCODE side "
                      "alone"),
                  "encode " + U32(stated.encodeBitDepthLuma) + " (asked " +
                      U32(request) + "), input " + U32(stated.inputBpp) +
                      " (was " + U32(derived.inputBpp) + ")");
            if (!fixtureOk) {
                continue;
            }

            const uint32_t wantEncode =
                c.want(stated.encodeChromaSubsampling, request);
            const uint32_t wantInput =
                c.want(stated.encodeChromaSubsampling, stated.inputBpp);
            if (wantEncode == kNoAdmissibleProfile) {
                excluded++;
                continue;
            }
            requested++;
            if (wantEncode != wantInput) {
                divergent[ci]++;
            }
            Check(ProfileAdmitsGeometry(c.codec, wantEncode,
                                        stated.encodeChromaSubsampling,
                                        request),
                  Lbl(std::string(c.name) + " enumerant " +
                      U32((uint32_t)routable[i]) +
                      ": the stated-depth expectation is in spec too"),
                  "expects " + U32(wantEncode) + " at " + U32(request) +
                      "-bit");
            Check(stated.codecProfile == wantEncode,
                  Lbl(std::string(c.name) + " enumerant " +
                      U32((uint32_t)routable[i]) +
                      ": with the two sides DIFFERENT, the arm follows the "
                      "ENCODE side"),
                  "encode side is (subsampling " +
                      U32(stated.encodeChromaSubsampling) + ", " +
                      U32(request) + "-bit) implying profile " +
                      U32(wantEncode) + "; the INPUT side is " +
                      U32(stated.inputBpp) + "-bit implying " +
                      ((wantInput == kNoAdmissibleProfile)
                           ? std::string("no in-spec profile")
                           : U32(wantInput)) +
                      "; the arm derived " + U32(stated.codecProfile));
        }
    }
    std::printf("  STATED-DEPTH PASS [%s]: %u rows asserted, divergent per "
                "arm H.264=%u H.265=%u AV1=%u\n",
                g_currentCase, requested, divergent[0], divergent[1],
                divergent[2]);
    Check(requested >= 20u,
          "the stated-depth pass bound enough rows to be read",
          "asserted " + U32(requested));
    for (uint32_t ci = 0; ci < 3u; ci++) {
        // THE ANTI-TAUTOLOGY ASSERTION. Without this the pass above could be
        // green because the two sides never once implied different profiles,
        // which is precisely the condition that made the first pass a
        // non-guard.
        Check(divergent[ci] > 0u,
              Lbl(std::string(kCodecs[ci].name) +
                  ": the stated-depth pass contained rows where the two sides "
                  "imply DIFFERENT profiles, so it can discriminate"),
              "divergent rows " + U32(divergent[ci]));
    }
}

void CaseAv1SubsamplingMatchesTheDerivedSeqProfile()
{
    g_currentCase = "the AV1 sequence header's subsampling matches the "
                    "seq_profile the derivation chose";
    struct Row {
        VkFormat    fmt;
        uint32_t    wantProfile;   // StdVideoAV1Profile
        uint32_t    wantX;
        uint32_t    wantY;
        const char* what;
    };
    static const Row rows[] = {
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                  0u, 1u, 1u,
          "NV12 is Main (0) at (1, 1)" },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, 0u, 1u, 1u,
          "P010 is Main (0) at (1, 1) -- ten bits does not change seq_profile" },
        { VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                  1u, 0u, 0u,
          "NV24 is High (1), which REQUIRES (0, 0)" },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16, 1u, 0u, 0u,
          "S410 is High (1) at (0, 0)" },
        { VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,                  2u, 1u, 0u,
          "NV16 is Professional (2), which at ten bits or fewer is (1, 0)" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec       = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
        cfg.inputFormat = row.fmt;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, Lbl(std::string("bound: ") + row.what),
              "VkResult " + U32((uint32_t)r));
        if (r != VK_SUCCESS) {
            continue;
        }
        Check(probe.av1ColorConfigPresent == 1u,
              Lbl(std::string("a colour config was attached: ") + row.what),
              "av1ColorConfigPresent " + U32(probe.av1ColorConfigPresent));
        Check(probe.codecProfile == row.wantProfile,
              Lbl(std::string("the derivation picked its seq_profile: ") +
                  row.what),
              "got " + U32(probe.codecProfile) + ", want " +
                  U32(row.wantProfile));
        Check((probe.av1SubsamplingX == row.wantX) &&
                  (probe.av1SubsamplingY == row.wantY),
              Lbl(row.what),
              "got (" + U32(probe.av1SubsamplingX) + ", " +
                  U32(probe.av1SubsamplingY) + "), want (" + U32(row.wantX) +
                  ", " + U32(row.wantY) + ")");
    }
}

// ITU-T H.265 TABLE A.8, WHICH THE 4:4:4 ARM COULD NOT REACH.
//
// GetCpbVclFactor() assigned encodeChromaSubsampling -- a
// VkVideoChromaSubsamplingFlagBitsKHR, 0x2 / 0x4 / 0x8 -- into a variable
// called chroma_format_idc and then tested it against the VALUE 3. No real
// input makes 0x2, 0x4 or 0x8 equal 3, so every stream took the 4:2:0 factor
// of 1000, including the 4:4:4 ones that Table A.8 gives 2000 at eight bits
// and 2500 at ten.
//
// THE FACTOR IS READ DIRECTLY, and that is the point of projecting it. A
// too-high level is still a LEGAL level, which is why nothing caught this, so
// the level alone would be a weak instrument; and the plan's other candidate,
// the default vbvBufferSize, is not readable device-free at all -- the probe
// projects the CONFIG field, and InitRateControl, which is what computes the
// default from the factor, runs later and needs a session. It reads 0 here
// under the broken factor and under the fixed one alike.
//
// THE LEVEL AND TIER ARE STILL READ, at a bitrate that makes them BITE.
// IsSuitableLevel tests averageBitrate against maxBitRateMainTier x cpbFactor,
// and when main tier will not carry the bitrate DetermineLevelTier does not
// climb to the next level -- it takes HIGH TIER at the same one. So at 16
// Mbit/s on 1080p a 4:4:4 session sits at level 4.0 MAIN tier under the correct
// factor (12000 x 2000 = 24 Mbit/s) and at level 4.0 HIGH tier under the broken
// one (12000 x 1000 = 12 Mbit/s). The LEVEL is 4.0 either way, which is exactly
// why it is read together with the tier and never alone.
//
// At the default 4 Mbit/s neither ceiling binds and both terms are
// picture-size-bound, which is why the rows below carry three bitrates: one
// where the selection cannot move, and two where it must.
//
// THE DEPTH TERM IS NOW LIVE AT THIS CALL SITE TOO, and the rows read it at two
// chroma formats so that "the depth term works" is distinguishable from "the
// 4:4:4 cell was edited". Table A.8, for the formats this tree can reach:
//
//     4:2:0  8-bit  Main            1000
//     4:2:0 10-bit  Main 10         1000
//     4:2:0 12-bit  Main 12         1500
//     4:4:4  8-bit  Main 4:4:4      2000
//     4:4:4 10-bit  Main 4:4:4 10   2500
//
// TWO PAIRS OF ROWS ISOLATE IT AT A FIXED BITRATE AND A FIXED CHROMA. At 16
// Mbit/s a 4:2:0 stream sits at level 4.0 HIGH tier at eight bits (12000 x 1000
// = 12 Mbit/s, exceeded) and at level 4.0 MAIN tier at twelve (12000 x 1500 =
// 18 Mbit/s, not exceeded). At 28 Mbit/s a 4:4:4 stream sits at HIGH tier at
// eight bits (12000 x 2000 = 24 Mbit/s, exceeded) and at MAIN tier at ten
// (12000 x 2500 = 30 Mbit/s, not exceeded). In each pair the only variable is
// the depth, so a factor that ignored the depth would read the same tier for
// both members and a factor that had simply been raised everywhere would move
// the eight-bit member too.
//
// MAIN TIER IS THE CORRECT ANSWER WHERE IT IS ASSERTED, not merely a different
// one. H.265 Annex A selects the lowest tier and level whose limits the stream
// satisfies; high tier at level 4.0 is also conformant for these streams and is
// a stricter claim on the decoder than the bitstream needs.
void CaseH265CpbVclFactorFollowsTheChromaFormat()
{
    g_currentCase = "the H.265 CPB VCL factor is Table A.8's, per chroma "
                    "format and depth";
    // wantLevel is StdVideoH265LevelIdc: 5 is 4.0. wantTier is
    // general_tier_flag: 0 main, 1 high.
    struct Row {
        VkFormat    fmt;
        uint32_t    bitrate;
        uint32_t    wantFactor;
        uint32_t    wantLevel;
        uint32_t    wantTier;
        const char* what;
    };
    static const Row rows[] = {
        // THE CONTROLS, AND THEY RUN FIRST. 4:2:0 reads 1000 and makes the
        // same selection at BOTH bitrates.
        // Without them, "fixed the 4:4:4 arm" is indistinguishable from
        // "changed the factor everywhere".
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   4000000u,
          1000u, 5u, 0u, "8-bit 4:2:0 is 1000, at level 4.0 main tier" },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,  4000000u,
          1000u, 5u, 0u,
          "10-bit 4:2:0 is 1000 -- Table A.8's depth term is +500 per two bits "
          "ABOVE ten, so ten bits adds nothing" },
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                  16000000u,
          1000u, 5u, 1u,
          "8-bit 4:2:0 at 16 Mbit/s needs HIGH tier, which the factor does not "
          "change" },
        // THE CHROMA ARM.
        { VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                   4000000u,
          2000u, 5u, 0u, "8-bit 4:4:4 (NV24) is 2000" },
        { VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                  16000000u,
          2000u, 5u, 0u,
          "8-bit 4:4:4 at 16 Mbit/s fits level 4.0 MAIN tier on the right "
          "chroma factor" },
        // THE DEPTH ARM, AT 4:2:0. Table A.8 gives Main 12 the factor 1500,
        // which is the base 1000 plus one +500 step for the two bits above
        // ten. The pair at 16 Mbit/s is where it bites: the eight-bit row
        // three above takes HIGH tier at the same bitrate and this one does
        // not, and the depth is the only difference between them.
        { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,  4000000u,
          1500u, 5u, 0u, "12-bit 4:2:0 (P012) is 1500" },
        { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, 16000000u,
          1500u, 5u, 0u,
          "12-bit 4:2:0 at 16 Mbit/s fits level 4.0 MAIN tier, where 8-bit "
          "4:2:0 at the same bitrate does not" },
        // THE DEPTH ARM, AT 4:4:4, where the base factor moves as well as the
        // step: Table A.8 gives Main 4:4:4 10 the factor 2500 against Main
        // 4:4:4's 2000.
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,  4000000u,
          2500u, 5u, 0u, "10-bit 4:4:4 (S410) is 2500" },
        // AND THE PAIR THAT MOVES THE TIER. 28 Mbit/s is above main tier's
        // ceiling at 2000 (24 Mbit/s) and below it at 2500 (30 Mbit/s), so the
        // eight-bit member must still take HIGH tier and the ten-bit member
        // must not. Read together they say the depth term moved the selection;
        // read alone either would only say the selection is bitrate-sensitive.
        { VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                  28000000u,
          2000u, 5u, 1u,
          "8-bit 4:4:4 at 28 Mbit/s still needs HIGH tier at level 4.0" },
        { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16, 28000000u,
          2500u, 5u, 0u,
          "10-bit 4:4:4 at 28 Mbit/s fits level 4.0 MAIN tier, where 8-bit "
          "4:4:4 at the same bitrate does not" },
    };
    for (const Row& row : rows) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec          = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        cfg.inputFormat    = row.fmt;
        cfg.averageBitrate = row.bitrate;
        VkEncBoundConfigProbe probe{};
        const VkResult r = VkEncBuildAndProbeConfig(
            cfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &probe);
        Check(r == VK_SUCCESS, Lbl(std::string("bound: ") + row.what),
              "VkResult " + U32((uint32_t)r));
        if (r != VK_SUCCESS) {
            continue;
        }
        Check(probe.h265CpbVclFactor == row.wantFactor, Lbl(row.what),
              "got " + U32(probe.h265CpbVclFactor) + ", want " +
                  U32(row.wantFactor));
        Check((probe.h265LevelIdc == row.wantLevel) &&
                  (probe.h265GeneralTierFlag == row.wantTier),
              Lbl(std::string("and the level and tier it selects: ") +
                  row.what),
              "got StdVideoH265LevelIdc " + U32(probe.h265LevelIdc) +
                  " tier " + U32(probe.h265GeneralTierFlag) + ", want " +
                  U32(row.wantLevel) + " tier " + U32(row.wantTier));
    }
}

// THE BIND SET IS THE STANDARD'S LIMITS TABLE, AND THIS IS THE REACH CHECK.
//
// The derivation already selects H.264 High 4:4:4 Predictive, H.265 Range
// Extensions and AV1 High from 4:4:4 input, and the library emits those
// streams, so naming the same number explicitly must not be refused as
// "unbindable". The bind set covers every number the limits table states,
// and the standard's own limits refuse what the standard forbids.
//
// WHY A PAIR AND NOT AN ACCEPTANCE. VkEncBuildAndProbeConfig returns
// VK_ERROR_INITIALIZATION_FAILED for BOTH the old unbindable refusal and the
// limits refusal, so a bare "it is refused" assertion cannot say which line
// produced it. Each row below is therefore two configs on ONE profile whose
// limits row is NARROW: one input the profile admits, one it does not. Only an
// arm that both binds the number AND calls the limits guard makes both true.
//
// NOT 244, AND NOT H.265 4. Both admit 4:2:0 as well as 4:4:4, so no input
// format makes either refuse on subsampling and the pair would collapse into a
// single assertion. AV1 High is 4:4:4 ONLY and H.264 High 10 is 4:2:0 ONLY,
// which is what makes them discriminating.
void CaseWidenedBindSetStillRunsTheLimitsGuard()
{
    g_currentCase = "a newly bindable profile still refuses what the standard "
                    "denies it";
    struct Pair {
        VkVideoCodecOperationFlagBitsKHR codec;
        uint32_t                         profile;
        VkFormat                         admitted;
        const char*                      admittedWhy;
        VkFormat                         denied;
        const char*                      deniedWhy;
    };
    static const Pair pairs[] = {
        { VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
          STD_VIDEO_AV1_PROFILE_HIGH,
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          "AV1 High (1) over 4:4:4 (NV24) binds seq_profile 1",
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          "AV1 High (1) over 4:2:0 (NV12) is refused -- High is 4:4:4 only" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          STD_VIDEO_H264_PROFILE_IDC_HIGH_10,
          VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
          "H.264 High 10 (110) over 4:2:0 (NV12) binds profile_idc 110",
          VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
          "H.264 High 10 (110) over 4:4:4 (NV24) is refused -- 110 is 4:2:0 "
          "only" },
    };
    for (const Pair& p : pairs) {
        VkVideoEncoderConfig okCfg = BaseConfig();
        okCfg.codec       = p.codec;
        okCfg.profile     = p.profile;
        okCfg.inputFormat = p.admitted;
        VkEncBoundConfigProbe okProbe{};
        const VkResult okR =
            VkEncBuildAndProbeConfig(okCfg, p.codec, &okProbe);
        Check(okR == VK_SUCCESS, Lbl(std::string("accepted: ") + p.admittedWhy),
              "VkResult " + U32((uint32_t)okR));
        Check(okProbe.codecProfile == p.profile,
              Lbl(std::string("and binds the number it was given: ") +
                  p.admittedWhy),
              "got " + U32(okProbe.codecProfile));

        VkVideoEncoderConfig badCfg = BaseConfig();
        badCfg.codec       = p.codec;
        badCfg.profile     = p.profile;
        badCfg.inputFormat = p.denied;
        VkEncBoundConfigProbe badProbe{};
        const VkResult badR =
            VkEncBuildAndProbeConfig(badCfg, p.codec, &badProbe);
        Check(badR == VK_ERROR_INITIALIZATION_FAILED,
              Lbl(std::string("refused: ") + p.deniedWhy),
              "VkResult " + U32((uint32_t)badR));
    }

    // THE MIRROR CONTROL. A change that widened the LIMITS table rather than
    // the bind set, or that stopped refusing altogether, would pass everything
    // above. H.265 Main (1) is 4:2:0 only and must still refuse 4:4:4, and it
    // is bindable.
    VkVideoEncoderConfig mainCfg = BaseConfig();
    mainCfg.codec       = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    mainCfg.profile     = VK_VIDEO_ENCODER_PROFILE_H265_MAIN;
    mainCfg.inputFormat = VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
    VkEncBoundConfigProbe mainProbe{};
    const VkResult mainR = VkEncBuildAndProbeConfig(
        mainCfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &mainProbe);
    Check(mainR == VK_ERROR_INITIALIZATION_FAILED,
          "H.265 Main (1) over 4:4:4 is STILL refused -- the bind set widened, "
          "the standard's limits did not",
          "VkResult " + U32((uint32_t)mainR));

    // AND A NUMBER THE TABLE STATES NOTHING ABOUT IS STILL UNBINDABLE, so
    // "widened" is not "opened". 88 is not an H.264 profile_idc.
    VkVideoEncoderConfig junkCfg = BaseConfig();
    junkCfg.codec       = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    junkCfg.profile     = 88u;
    junkCfg.inputFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    VkEncBoundConfigProbe junkProbe{};
    const VkResult junkR = VkEncBuildAndProbeConfig(
        junkCfg, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &junkProbe);
    Check(junkR == VK_ERROR_INITIALIZATION_FAILED,
          "an H.264 profile_idc the standard's table does not state is still "
          "unbindable",
          "VkResult " + U32((uint32_t)junkR));
}

// EVERY PROFILE THE BINDER BINDS IS ONE THE PUBLIC HEADER NAMES, AND THE
// CONVERSE.
//
// The header's profile block says of its constants that "they are what this
// library binds today", and the sentence beside VkVideoEncoderConfig::profile
// routes a caller there "for the numbering and for what this library binds".
// Those were enumeration claims that nothing enforced: the bind set widened
// from six numbers to fourteen and the block did not move, so the header
// advertised a NARROWER set than the library accepted and the only way to ask
// for the difference was a bare integer. The tree's own point-query row for
// High 4:4:4 Predictive wrote 244u for exactly that reason. This case is what
// enforces them now, in both directions.
//
// SWEPT, NOT LISTED. A table of the fourteen checked against a table of the
// fourteen would agree with itself. This walks the whole value space each
// codec's syntax element can carry -- profile_idc and general_profile_idc are
// u(8), seq_profile is f(3) -- and asks the binder about every number in it,
// so a value added to the switch and not to the header fails here on the first
// run rather than on the first consumer.
//
// BINDABILITY IS "SOME INPUT ADMITS IT", because the second half of the
// binder's rule is the standard's limits table and most numbers are refused by
// it on 4:2:0 8-bit input. Six inputs span that table's whole domain: 8, 10
// and 12 bits at 4:2:0, and 4:2:2 and 4:4:4. A number no input admits is not a
// number a caller can use.
//
// THE OVERLAP IS STATED, NOT WORKED AROUND. On AV1, 0 is seq_profile Main and
// is also VK_VIDEO_ENCODER_PROFILE_DEFAULT, so the binder never sees it and
// the derivation produces it; the sweep reads it as bound because a caller
// that writes 0 does get seq_profile 0, which is what the constant promises.
// On H.264 and H.265, 0 is DEFAULT alone and the derivation lands elsewhere,
// so it reads as unbound there.
struct NamedProfile {
    uint32_t    value;
    const char* spelling;
};

// The public header's own enumeration, transcribed. This table is the header's
// claim; the sweep below is the code's answer.
const NamedProfile kNamedH264[] = {
    { VK_VIDEO_ENCODER_PROFILE_H264_BASELINE,
      "VK_VIDEO_ENCODER_PROFILE_H264_BASELINE" },
    { VK_VIDEO_ENCODER_PROFILE_H264_MAIN,
      "VK_VIDEO_ENCODER_PROFILE_H264_MAIN" },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH,
      "VK_VIDEO_ENCODER_PROFILE_H264_HIGH" },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10,
      "VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10" },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH_422,
      "VK_VIDEO_ENCODER_PROFILE_H264_HIGH_422" },
    { VK_VIDEO_ENCODER_PROFILE_H264_HIGH_444_PREDICTIVE,
      "VK_VIDEO_ENCODER_PROFILE_H264_HIGH_444_PREDICTIVE" },
};
const NamedProfile kNamedH265[] = {
    { VK_VIDEO_ENCODER_PROFILE_H265_MAIN,
      "VK_VIDEO_ENCODER_PROFILE_H265_MAIN" },
    { VK_VIDEO_ENCODER_PROFILE_H265_MAIN10,
      "VK_VIDEO_ENCODER_PROFILE_H265_MAIN10" },
    { VK_VIDEO_ENCODER_PROFILE_H265_MAIN_STILL_PICTURE,
      "VK_VIDEO_ENCODER_PROFILE_H265_MAIN_STILL_PICTURE" },
    { VK_VIDEO_ENCODER_PROFILE_H265_FORMAT_RANGE_EXTENSIONS,
      "VK_VIDEO_ENCODER_PROFILE_H265_FORMAT_RANGE_EXTENSIONS" },
    { VK_VIDEO_ENCODER_PROFILE_H265_SCC_EXTENSIONS,
      "VK_VIDEO_ENCODER_PROFILE_H265_SCC_EXTENSIONS" },
};
const NamedProfile kNamedAv1[] = {
    { VK_VIDEO_ENCODER_PROFILE_AV1_MAIN,
      "VK_VIDEO_ENCODER_PROFILE_AV1_MAIN" },
    { VK_VIDEO_ENCODER_PROFILE_AV1_HIGH,
      "VK_VIDEO_ENCODER_PROFILE_AV1_HIGH" },
    { VK_VIDEO_ENCODER_PROFILE_AV1_PROFESSIONAL,
      "VK_VIDEO_ENCODER_PROFILE_AV1_PROFESSIONAL" },
};

// Does |profile| bind on |codec| for at least one input the standard's limits
// table admits?
bool ProfileBindsOnSomeInput(VkVideoCodecOperationFlagBitsKHR codec,
                             uint32_t                         profile)
{
    static const VkFormat kSpan[] = {
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   // 8-bit 4:2:0
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,  // 10-bit 4:2:0
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,  // 12-bit 4:2:0
        VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,                   // 8-bit 4:2:2
        VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,                   // 8-bit 4:4:4
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16,  // 10-bit 4:4:4
    };
    for (VkFormat fmt : kSpan) {
        VkVideoEncoderConfig cfg = BaseConfig();
        cfg.codec       = codec;
        cfg.profile     = profile;
        cfg.inputFormat = fmt;
        VkEncBoundConfigProbe probe{};
        if (VkEncBuildAndProbeConfig(cfg, codec, &probe) != VK_SUCCESS) {
            continue;
        }
        if (probe.codecProfile == profile) {
            return true;
        }
    }
    return false;
}

void SweepOneCodecsProfileSpace(VkVideoCodecOperationFlagBitsKHR codec,
                                const char*                      codecName,
                                const NamedProfile*              named,
                                size_t                           namedCount,
                                uint32_t                         valueSpace)
{
    // Half one: every constant the header names must bind. A name for a value
    // the library refuses is the same defect pointing the other way.
    for (size_t i = 0; i < namedCount; i++) {
        Check(ProfileBindsOnSomeInput(codec, named[i].value),
              Lbl(std::string(named[i].spelling) + " (" +
                  U32(named[i].value) + ") is a profile this library binds"),
              "no input in the span bound it");
    }

    // Half two: nothing outside the named set binds.
    std::string unnamed;
    uint32_t    unnamedCount = 0;
    for (uint32_t v = 0; v < valueSpace; v++) {
        bool isNamed = false;
        for (size_t i = 0; i < namedCount; i++) {
            if (named[i].value == v) {
                isNamed = true;
                break;
            }
        }
        if (isNamed || !ProfileBindsOnSomeInput(codec, v)) {
            continue;
        }
        unnamedCount++;
        if (!unnamed.empty()) {
            unnamed += ", ";
        }
        unnamed += U32(v);
    }
    Check(unnamedCount == 0,
          Lbl(std::string(codecName) +
              ": the header names every profile the binder binds"),
          "bindable and unnamed: " + unnamed);
}

void CaseNamedProfileConstantsAreExactlyTheBoundSet()
{
    g_currentCase = "the named profile constants are exactly what the binder "
                    "binds";

    // CALIBRATION, BEFORE THE SWEEP READS ANYTHING. The sweep's verdict is a
    // membership test, and a membership test that answered the same for every
    // input would report a clean set-equality no matter what the binder did.
    // One number known to bind and one known not to, through the same
    // function, on the same inputs.
    Check(ProfileBindsOnSomeInput(
              VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
              VK_VIDEO_ENCODER_PROFILE_H264_HIGH),
          "calibration: the sweep reads H.264 High (100) as BOUND",
          "the instrument cannot see a bound profile");
    Check(!ProfileBindsOnSomeInput(
              VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 88u),
          "calibration: the sweep reads 88, which is no profile_idc, as "
          "UNBOUND",
          "the instrument reports everything bound");

    // profile_idc and general_profile_idc are u(8); seq_profile is f(3).
    SweepOneCodecsProfileSpace(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
                               "H.264", kNamedH264,
                               sizeof(kNamedH264) / sizeof(kNamedH264[0]),
                               256u);
    SweepOneCodecsProfileSpace(VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
                               "H.265", kNamedH265,
                               sizeof(kNamedH265) / sizeof(kNamedH265[0]),
                               256u);
    SweepOneCodecsProfileSpace(VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
                               "AV1", kNamedAv1,
                               sizeof(kNamedAv1) / sizeof(kNamedAv1[0]),
                               8u);
}

//=============================================================================
// 2b. What the capability probe can ASK a driver
//
// The probe is keyed on (codec, profile, bit depth). These assert the KEY, not
// a device answer: this runner has no encode-capable device, so no capability
// entry point can answer anything but "not present", and a claim about what a
// driver reports would be unfounded.
//=============================================================================

void CaseProbeNamesAv1MainAtBothDepths()
{
    g_currentCase = "the probe can name AV1 Main at 8 AND at 10 bits";
    // AV1 seq_profile 0 carries 8 or 10 bits at 4:2:0 (AV1 A.2). One profile,
    // two VkVideoProfileInfoKHR values, so both have to be nameable or the
    // 10-bit half of a profile that encodes on hardware today cannot be asked
    // about at all.
    Check(VkEncProbeNamesProfileBitDepth(
              VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
              VK_VIDEO_ENCODER_PROFILE_AV1_MAIN, 8),
          "AV1 Main at 8 bits", "not named");
    Check(VkEncProbeNamesProfileBitDepth(
              VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
              VK_VIDEO_ENCODER_PROFILE_AV1_MAIN, 10),
          "AV1 Main at 10 bits", "not named");
    // The control that makes the two above measure the DEPTH term rather than
    // a table that says yes to everything: a depth no arm carries.
    Check(!VkEncProbeNamesProfileBitDepth(
              VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
              VK_VIDEO_ENCODER_PROFILE_AV1_MAIN, 12),
          "AV1 Main at 12 bits is NOT named", "unexpectedly named");
}

void CaseProbeRefusesDepthsItHasNoEvidenceFor()
{
    g_currentCase = "a profile is probed only at the depth it is probed at";
    struct Row { VkVideoCodecOperationFlagBitsKHR codec; uint32_t profile;
                 uint32_t depth; bool named; const char* why; };
    static const Row rows[] = {
        // H.264 Baseline, Main and High are 8-bit (H.264 A.2).
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH, 8, true,
          "H.264 High at 8 bits" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H264_HIGH, 10, false,
          "H.264 High at 10 bits" },
        // High 10 (profile_idc 110) is a row at 10 bits only. That is
        // narrower than H.264 A.2.5 allows -- 110 admits 8-bit as well --
        // and is deliberate, the same choice the H.265 Main 10 rows below
        // make: 8-bit input has High, so the 8-bit 110 pairing is not a row.
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 110, 10, true,
          "H.264 High 10 at 10 bits" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 110, 8, false,
          "H.264 High 10 at 8 bits" },
        // H.265 Main is 8-bit 4:2:0 (A.3.2); Main 10 is probed at 10 only,
        // which is narrower than A.3.3 allows and is deliberate.
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN, 8, true,
          "H.265 Main at 8 bits" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN, 10, false,
          "H.265 Main at 10 bits" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN10, 10, true,
          "H.265 Main 10 at 10 bits" },
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN10, 8, false,
          "H.265 Main 10 at 8 bits" },
        // The codec arm still disambiguates a repeated number: 1 is H.265
        // Main and is not an H.264 profile_idc.
        { VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
          VK_VIDEO_ENCODER_PROFILE_H265_MAIN, 8, false,
          "H.265 Main number on the H.264 arm" },
    };
    for (const Row& row : rows) {
        const bool named = VkEncProbeNamesProfileBitDepth(
            row.codec, row.profile, row.depth);
        Check(named == row.named,
              (std::string(row.named ? "named: " : "not named: ") +
               row.why).c_str(),
              named ? "named" : "not named");
    }
}

void CaseSnapshotCarriesBothAv1Depths()
{
    g_currentCase = "the context snapshot holds a row per probed depth";
    // The rows are what a context build issues one driver query each for, so
    // this is what decides whether the AV1 10-bit question ever reaches a
    // driver at all.
    uint32_t av1Rows = VkEncProbeSnapshotRowCount(
        VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);
    Check(av1Rows == 2, "AV1 has two probe rows", "got " + U32(av1Rows));

    bool sawEight = false;
    bool sawTen   = false;
    uint32_t firstProfile = 0;
    uint32_t firstDepth   = 0;
    for (uint32_t slot = 0; slot < av1Rows; slot++) {
        uint32_t profile = 0;
        uint32_t depth   = 0;
        if (!VkEncProbeSnapshotRowAt(
                VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, slot,
                &profile, &depth)) {
            Check(false, "row readable", "slot " + U32(slot));
            continue;
        }
        Check(profile == VK_VIDEO_ENCODER_PROFILE_AV1_MAIN,
              "every AV1 row is seq_profile 0", "got " + U32(profile));
        if (slot == 0) {
            firstProfile = profile;
            firstDepth   = depth;
        }
        sawEight = sawEight || (depth == 8);
        sawTen   = sawTen   || (depth == 10);
    }
    Check(sawEight, "an 8-bit AV1 Main row", "absent");
    Check(sawTen,   "a 10-bit AV1 Main row", "absent");
    // Order is load-bearing: the public lookup resolves a profile number to
    // the FIRST row carrying it, so the 8-bit row has to be first or every
    // published AV1 answer would silently become the 10-bit one.
    Check((firstProfile == VK_VIDEO_ENCODER_PROFILE_AV1_MAIN) &&
              (firstDepth == 8),
          "the 8-bit row is first, so published answers are unchanged",
          "profile " + U32(firstProfile) + " depth " + U32(firstDepth));

    // The other two codecs are pinned by count as well, so a row that
    // appears or vanishes fails here rather than silently changing what the
    // library probes a driver for.
    const uint32_t h264Rows = VkEncProbeSnapshotRowCount(
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR);
    const uint32_t h265Rows = VkEncProbeSnapshotRowCount(
        VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR);
    Check(h264Rows == 5, "H.264 has five rows", "got " + U32(h264Rows));
    Check(h265Rows == 3, "H.265 still has three rows", "got " + U32(h265Rows));
    // A codec with no rows answers zero rather than reading off the end.
    const uint32_t noneRows =
        VkEncProbeSnapshotRowCount(VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    Check(noneRows == 0, "an unprobed codec has no rows",
          "got " + U32(noneRows));
    Check(!VkEncProbeSnapshotRowAt(
              VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, av1Rows,
              nullptr, nullptr),
          "one past the last AV1 row is refused", "accepted");
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
    // produces, and it must not emit BT.709 chroma under BT.2020 primaries.
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
    CaseAdvertisedListDropsAnUnreachableConversionTarget();
    CaseOptimalityNamesTheEncodersOwnFormat();
    CaseFourFourFourReachesTheListOnlyFromTheDevice();
    CaseFilterlessBuildAdvertisesNoConvertedEntry();
    CaseConversionTargetPreservesSubsamplingAndDepth();
    CaseRoutableListAgreesWithTheClassifier();
    CaseRoutableSetIsDerivedFromTheFormatTables();
    CaseSinglePlaneInterleavedIsRefused();
    CaseFourTwoZeroDeviceAdvertisesTheHistoricalSet();
    CaseYcbcrIsNeverClaimedAsRgba();
    CaseRgbaSessionSurvivesTheSinglePlaneGate();

    CaseContradictoryColorModelIsRefusedByTheBinder();
    CaseAgreeingColorModelStillBinds();
    CaseSemiPlanarBindsTwoPlanes();
    CaseInputFormatSurvivesTheRoundTripThroughGeometry();
    CaseTenBitBindsBitDepthAndPlanes();
    CaseFourFourFourBindsItsOwnSubsampling();
    CaseFourTwoZeroStillBindsFourTwoZero();
    CaseDirectFormatGetsNoFilter();
    CaseThreePlaneGetsAFilterWithoutAsking();
    CaseRgbaGetsAFilterWithoutAsking();
    CaseUnsupportedFormatStillRefused();
    CasePackedYcbcrIsRoutedWhereItIsDeclared();
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
    CaseProfileMustAdmitTheInputSubsampling();
    CaseNamedProfileConstantsAreExactlyTheBoundSet();
    CaseInputColourChainBindsEachAxis();
    CaseAbsentInputColourChainChangesNothing();
    CaseInputColourDisagreementIsRefused();
    CaseDeclaredInputRangeDecidesTheStreamsRange();
    CaseInputColourPrimariesDriveTheDerivedMatrix();
    CaseAv1SubsamplingMatchesTheDerivedSeqProfile();
    CaseH265CpbVclFactorFollowsTheChromaFormat();
    CaseWidenedBindSetStillRunsTheLimitsGuard();
    CaseProbeNamesAv1MainAtBothDepths();
    CaseProbeRefusesDepthsItHasNoEvidenceFor();
    CaseSnapshotCarriesBothAv1Depths();

    CaseCodecArmsDeriveTheProfileFromTheEncodeGeometry();

    CaseFieldTableClassifiesEveryField();

    std::printf("-------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
