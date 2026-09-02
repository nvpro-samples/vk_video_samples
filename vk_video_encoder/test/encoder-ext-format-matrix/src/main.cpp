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
 * INPUT-FORMAT MATRIX, on the LIBRARY-OWNED device.
 *
 * WHAT THIS ANSWERS. VkEncClassifyInput names eleven inputs: four
 * ENCODABLE_DIRECT (NV12, P010), four ENCODABLE_VIA_FILTER YCbCr (P012 and
 * I420 8/10/12-bit) and three ENCODABLE_VIA_FILTER RGBA (R8G8B8A8_UNORM,
 * B8G8R8A8_UNORM, A8B8G8R8_UNORM_PACK32). "Claims to support" is a statement
 * about that table. Whether a format REGISTERS, and onto WHICH INPUT PATH, is
 * a statement about a device, a session and a descriptor -- three things the
 * table does not see. This walks all nine against a real device and reports
 * both, with the denominator, so a format that does NOT encode is a recorded
 * result rather than a missing row.
 *
 * WHAT IT CAN FAIL ON -- stated up front, because a suite whose assertions
 * cannot discriminate has shipped on this project before:
 *
 *   - It asserts the RGBA rows REGISTER and resolve to
 *     VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER. A library that refuses them
 *     answers ERROR_CONVERSION_REQUIRED at registration and this fails.
 *   - It asserts the routing from VkEncProbeResource -- the slot's own
 *     resolved inputPath -- and NOT from the config flag, from
 *     ComputeFilterActive(), or from a log line. Those can all be true of a
 *     registration that resolved to STAGED.
 *   - It asserts the gate still DISCRIMINATES: the same RGBA image registered
 *     with a descriptor that does not grant VK_IMAGE_USAGE_STORAGE_BIT must
 *     still be refused CONVERSION_REQUIRED. A fix that simply deleted the
 *     refusal would pass every "RGBA registers" assertion and fail this one.
 *   - It asserts the multi-planar arm is UNCHANGED: a 3-plane descriptor
 *     without MUTABLE_FORMAT|EXTENDED_USAGE|STORAGE is still refused, and
 *     with them still resolves to FILTER through planeStorageViews, not
 *     through the new single-plane fact.
 *   - It asserts that Reconfigure REFUSES a config whose inputColorModel
 *     resolves to the other model, with two controls that must still be
 *     accepted: a rate-control change, and the same model stated explicitly
 *     rather than left to the format. A library that discards the field
 *     answers VK_SUCCESS to all three.
 *
 * WHY REGISTRATION AND ROUTING ARE REPORTED SEPARATELY. Registration tests
 * the DESCRIPTOR; routing tests the VIEW that was actually built. They can
 * disagree, and the disagreement is not academic: it is how a filter-only
 * format could reach the staging copy, which for such a format is a measured
 * VK_ERROR_DEVICE_LOST. Every row prints both.
 */

#include "vulkan_video_encoder_ext.h"
#include "vulkan_video_encoder_ext_internal.h"

#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;

int g_failures = 0;
int g_checks   = 0;

void Check(const char* row, bool ok, const char* what,
           const std::string& detail)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("    FAIL [%s] %s\n           %s\n", row, what,
                    detail.c_str());
    } else {
        std::printf("    ok   [%s] %s\n", row, what);
    }
}

std::string U32(unsigned long long v) { return std::to_string(v); }

// ---------------------------------------------------------------------------
// Vulkan entry points, loaded off the LIBRARY's instance/device. Nothing here
// creates a device of its own: the whole point is the library-owned one.
// ---------------------------------------------------------------------------
struct DeviceFns {
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkMapMemory                         MapMemory = nullptr;
    PFN_vkUnmapMemory                       UnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
        nullptr;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties =
        nullptr;
};

bool LoadDeviceFns(VkInstance instance, VkDevice device, DeviceFns* fns)
{
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (lib == nullptr) {
        lib = dlopen("libvulkan.so", RTLD_NOW);
    }
    if (lib == nullptr) {
        std::printf("  ERROR: dlopen(libvulkan) failed: %s\n", dlerror());
        return false;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (gipa == nullptr) {
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (gdpa == nullptr) {
        return false;
    }
#define LOAD_DEV(name)                                                       \
    fns->name = (PFN_vk##name)gdpa(device, "vk" #name);                      \
    if (fns->name == nullptr) {                                              \
        std::printf("  ERROR: missing vk" #name "\n");                       \
        return false;                                                        \
    }
    LOAD_DEV(CreateImage)
    LOAD_DEV(DestroyImage)
    LOAD_DEV(GetImageMemoryRequirements)
    LOAD_DEV(AllocateMemory)
    LOAD_DEV(FreeMemory)
    LOAD_DEV(BindImageMemory)
    LOAD_DEV(MapMemory)
    LOAD_DEV(UnmapMemory)
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    fns->GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(
            instance, "vkGetPhysicalDeviceProperties");
    fns->GetPhysicalDeviceFormatProperties =
        (PFN_vkGetPhysicalDeviceFormatProperties)gipa(
            instance, "vkGetPhysicalDeviceFormatProperties");
    return (fns->GetPhysicalDeviceMemoryProperties != nullptr) &&
           (fns->GetPhysicalDeviceProperties != nullptr) &&
           (fns->GetPhysicalDeviceFormatProperties != nullptr);
}

// ---------------------------------------------------------------------------
// The nine formats the library claims, in taxonomy order, plus two controls.
// ---------------------------------------------------------------------------
enum Arm { ARM_DIRECT, ARM_FILTER_YCBCR, ARM_FILTER_RGBA, ARM_CONTROL };

// Rows are GROUPED by the video PROFILE their bit depth needs, and every
// group's DIRECT member is its control. "This format does not encode" then
// splits into two different findings that must not be conflated:
//
//   * the DEVICE has no such profile -- the group's DIRECT control fails to
//     initialize too, and the library never got a say; or
//   * the LIBRARY refuses it -- the control initializes and the filter
//     member does not, or registers and routes wrongly.
//
// Measured on the A4000 the first time this ran: all four 10/12-bit rows
// failed, and every one of them was
// GetPhysicalDeviceVideoCapabilitiesKHR answering
// VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR. Attributing that to the
// registration gate would have been simply wrong.
// The 4:4:4 groups are separate profiles, not a bit-depth variant of the
// 4:2:0 ones: H.264 High 4:4:4 Predictive and H.265 Range Extensions are
// what a semi-planar 4:4:4 encode source belongs to, and a device may
// expose one subsampling and not the other.
enum Group {
    G_8BIT   = 0,
    G_10BIT  = 1,
    G_12BIT  = 2,
    G_444_8  = 3,
    G_444_10 = 4,
    G_NONE   = 5
};

// The video profile a group's images and sessions belong to. Everything the
// profile list needs is a function of the group, so a row states its group
// and nothing else.
VkVideoChromaSubsamplingFlagBitsKHR GroupSubsampling(Group g)
{
    return ((g == G_444_8) || (g == G_444_10))
               ? VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR
               : VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
}

VkVideoComponentBitDepthFlagBitsKHR GroupDepth(Group g)
{
    switch (g) {
        case G_10BIT:
        case G_444_10: return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        case G_12BIT:  return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
        default:       return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    }
}

struct Row {
    const char* name;
    VkFormat    format;
    Arm         arm;
    Group       group;
    VkVideoCodecOperationFlagBitsKHR codec;
    const char* codecName;
};

// H.265 carries the 10- and 12-bit rows: H.264 High has no 10-bit 4:2:0
// profile at all on this stack, so testing depth through it measures the
// codec rather than the format.
#define H264 VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H264"
#define H265 VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H265"

const Row kRows[] = {
    {"NV12          G8_B8R8_2PLANE_420_UNORM",
     VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ARM_DIRECT, G_8BIT, H264},
    {"P010  G10X6_B10X6R10X6_2PLANE_420...",
     VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, ARM_DIRECT,
     G_10BIT, H265},
    {"P012  G12X4_B12X4R12X4_2PLANE_420...",
     VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, ARM_FILTER_YCBCR,
     G_12BIT, H265},
    {"I420          G8_B8_R8_3PLANE_420_UNORM",
     VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, ARM_FILTER_YCBCR, G_8BIT, H264},
    {"I420-10 G10X6_B10X6_R10X6_3PLANE_420",
     VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, ARM_FILTER_YCBCR,
     G_10BIT, H265},
    {"I420-12 G12X4_B12X4_R12X4_3PLANE_420",
     VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16, ARM_FILTER_YCBCR,
     G_12BIT, H265},
    {"RGBA8         R8G8B8A8_UNORM",
     VK_FORMAT_R8G8B8A8_UNORM, ARM_FILTER_RGBA, G_8BIT, H264},
    {"BGRA8         B8G8R8A8_UNORM",
     VK_FORMAT_B8G8R8A8_UNORM, ARM_FILTER_RGBA, G_8BIT, H264},
    {"ABGR8-packed  A8B8G8R8_UNORM_PACK32",
     VK_FORMAT_A8B8G8R8_UNORM_PACK32, ARM_FILTER_RGBA, G_8BIT, H264},
    // Semi-planar 4:4:4. Same layout and same depth as the 4:2:0 pair above,
    // a different profile: these belong to H.264 High 4:4:4 Predictive and
    // H.265 Range Extensions, and each is the only member of its group, so
    // its own initialization is what separates "this device has no 4:4:4
    // encode profile" from "the library refuses the format".
    {"NV24          G8_B8R8_2PLANE_444_UNORM",
     VK_FORMAT_G8_B8R8_2PLANE_444_UNORM, ARM_DIRECT, G_444_8, H264},
    {"S410  G10X6_B10X6R10X6_2PLANE_444...",
     VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16, ARM_DIRECT,
     G_444_10, H265},
    // Controls. Neither is in the taxonomy; both MUST be refused, and by the
    // format gate rather than by the arm gates.
    {"CONTROL       R8G8B8A8_SRGB (excluded)",
     VK_FORMAT_R8G8B8A8_SRGB, ARM_CONTROL, G_NONE, H264},
    {"CONTROL       R16G16B16A16_SFLOAT (excl)",
     VK_FORMAT_R16G16B16A16_SFLOAT, ARM_CONTROL, G_NONE, H264},
};
const size_t kNumRows = sizeof(kRows) / sizeof(kRows[0]);

const char* PathName(VkVideoEncoderExternalInputPath p)
{
    switch (p) {
        case VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT: return "DIRECT";
        case VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED: return "STAGED";
        case VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER: return "FILTER";
        default: return "?";
    }
}

const char* ClassName(VkEncInputFormatClass c)
{
    switch (c) {
        case VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT:     return "DIRECT";
        case VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER: return "VIA_FILTER";
        default:                                       return "UNSUPPORTED";
    }
}

struct Img {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool CreateImage(const DeviceFns& fns, VkPhysicalDevice phys, VkDevice device,
                 VkFormat format, VkImageTiling tiling,
                 VkImageUsageFlags usage, VkImageCreateFlags flags,
                 bool withProfileList,
                 VkVideoCodecOperationFlagBitsKHR codec,
                 VkVideoComponentBitDepthFlagBitsKHR depth,
                 VkVideoChromaSubsamplingFlagBitsKHR subsampling, Img* out)
{
    // The codec-specific profile struct is NOT optional:
    // VUID-VkVideoProfileInfoKHR-videoCodecOperation-07181/07182 require it,
    // and without it the whole profile-list query fails -- which then shows
    // up as -06811 and -02251 on the same create, three VUIDs deep, none of
    // them naming the actual omission.
    VkVideoEncodeH264ProfileInfoKHR h264Profile{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    // 4:4:4 is carried by a different profile in both codecs, and the image
    // has to name the same one its session will negotiate or the profile
    // list describes an image the encoder never sees.
    const bool is444 =
        (subsampling == VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR);
    h264Profile.stdProfileIdc =
        is444 ? STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE
              : STD_VIDEO_H264_PROFILE_IDC_HIGH;
    VkVideoEncodeH265ProfileInfoKHR h265Profile{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR};
    h265Profile.stdProfileIdc =
        is444 ? STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS
        : (depth == VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
            ? STD_VIDEO_H265_PROFILE_IDC_MAIN
            : STD_VIDEO_H265_PROFILE_IDC_MAIN_10;

    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    profile.pNext =
        (codec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)
            ? (void*)&h264Profile
            : (void*)&h265Profile;
    profile.videoCodecOperation = codec;
    profile.chromaSubsampling   = subsampling;
    profile.lumaBitDepth        = depth;
    profile.chromaBitDepth      = depth;
    VkVideoProfileListInfoKHR profileList{
        VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profileList.profileCount = 1;
    profileList.pProfiles    = &profile;

    // MUTABLE_FORMAT obliges a format list naming every view format
    // (VUID-VkImageCreateInfo-tiling-02353 / -pNext-01585), and every entry
    // must be compatible with a PLANE of this format
    // (VUID-VkImageCreateInfo-pNext-10062). So the list is derived from the
    // format rather than hardcoded: a 3-plane 420 image's planes are all
    // single-component, and offering it R8G8_UNORM is invalid even though
    // the library never asks for such a view.
    VkFormat viewFormats[4] = {format, VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    uint32_t viewFormatCount = 1;
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
            viewFormats[viewFormatCount++] = VK_FORMAT_R8_UNORM;
            break;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
            // BOTH spellings. The plane format of a G10X6 image is
            // R10X6_UNORM_PACK16, but VkImageResourceView::Create derives its
            // plane views as R16_UNORM -- class-compatible (both 16-bit
            // single-component) and therefore legal, but a format list naming
            // only the plane spelling makes the view VUID-...-pNext-01585.
            // Measured, not assumed: that VUID fired here three times before
            // R16_UNORM was added.
            viewFormats[viewFormatCount++] = VK_FORMAT_R10X6_UNORM_PACK16;
            viewFormats[viewFormatCount++] = VK_FORMAT_R16_UNORM;
            break;
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
            viewFormats[viewFormatCount++] = VK_FORMAT_R12X4_UNORM_PACK16;
            viewFormats[viewFormatCount++] = VK_FORMAT_R16_UNORM;
            break;
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
            viewFormats[viewFormatCount++] = VK_FORMAT_R8_UNORM;
            viewFormats[viewFormatCount++] = VK_FORMAT_R8G8_UNORM;
            break;
        default:
            break;
    }
    VkImageFormatListCreateInfo listInfo{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    listInfo.viewFormatCount = viewFormatCount;
    listInfo.pViewFormats    = viewFormats;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const void* chain = nullptr;
    if (withProfileList) {
        chain = &profileList;
    }
    if ((flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
        listInfo.pNext = chain;
        chain = &listInfo;
    }
    ci.pNext         = chain;
    ci.flags         = flags;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = format;
    ci.extent        = {kWidth, kHeight, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = tiling;
    ci.usage         = usage;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = (tiling == VK_IMAGE_TILING_LINEAR)
                           ? VK_IMAGE_LAYOUT_PREINITIALIZED
                           : VK_IMAGE_LAYOUT_UNDEFINED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    fns.GetImageMemoryRequirements(device, out->image, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    fns.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    const VkMemoryPropertyFlags want =
        (tiling == VK_IMAGE_TILING_LINEAR)
            ? (VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            : (VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((req.memoryTypeBits & (1u << i)) != 0) &&
            ((memProps.memoryTypes[i].propertyFlags & want) == want)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        fns.DestroyImage(device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (fns.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) {
        fns.DestroyImage(device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }
    if (fns.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS) {
        return false;
    }
    return true;
}

void DestroyImg(const DeviceFns& fns, VkDevice device, Img* img)
{
    if (img->image != VK_NULL_HANDLE) {
        fns.DestroyImage(device, img->image, nullptr);
        img->image = VK_NULL_HANDLE;
    }
    if (img->memory != VK_NULL_HANDLE) {
        fns.FreeMemory(device, img->memory, nullptr);
        img->memory = VK_NULL_HANDLE;
    }
}

// One row of the matrix. Everything is per-session because the format is a
// SESSION property: SupportsFormat, the filter's input format and the
// registration gate all read m_encoderConfig.
struct RowResult {
    bool sessionInit      = false;
    bool supportsFormat   = false;
    bool imageCreated     = false;
    VkBool32 filterCapableOptimal = VK_FALSE;
    VkBool32 filterCapableLinear  = VK_FALSE;
    VkVideoEncoderStatusCode queryStatus =
        VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED;
    VkBool32 querySupported = VK_FALSE;
    VkVideoEncoderStatusCode regStatus =
        VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED;
    VkVideoEncoderExternalInputPath path =
        VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    VkBool32 planeStorageViews = VK_FALSE;
    VkBool32 storageReadView   = VK_FALSE;
    // The negative control: the same image with the arm's declaration
    // withheld must still be refused.
    bool     ranWithheld    = false;
    VkVideoEncoderStatusCode withheldStatus =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;
    // The COLOUR-MODEL DECLARATION, asked on the same session and the same
    // image, so the declaration is the only thing that differs between these
    // and the row above.
    bool     ranDeclared = false;
    VkVideoEncoderStatusCode declaredAgreeing =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;
    VkVideoEncoderStatusCode declaredYcbcr =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;
    VkVideoEncoderStatusCode declaredYcbcrNoExtent =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;
    VkVideoEncoderStatusCode fromFormatNoExtent =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;
    // The SESSION-level counterpart of those declarations: what Reconfigure
    // does with the same pair. The two positives default to a value that
    // fails their check, so a leg that did not run cannot read as a pass.
    bool     ranReconfig      = false;
    VkResult reconfigBitrate  = VK_ERROR_UNKNOWN;
    VkResult reconfigAgreeing = VK_ERROR_UNKNOWN;
    VkResult reconfigYcbcr    = VK_SUCCESS;
};

// The declaration each arm's descriptor makes. This is the whole content of
// the fix under test, expressed as data.
struct Decl {
    VkImageUsageFlags  usage;
    VkImageCreateFlags flags;
    VkImageTiling      tiling;
    bool               profileList;
};

Decl DeclFor(Arm arm, bool withhold)
{
    Decl d = {};
    switch (arm) {
        case ARM_DIRECT:
            d.tiling = VK_IMAGE_TILING_OPTIMAL;
            d.usage  = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            d.profileList = true;
            if (withhold) {  // no VIDEO_ENCODE_SRC -> not encodeCapable
                d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                d.profileList = false;
            }
            break;
        case ARM_FILTER_YCBCR:
            d.tiling = VK_IMAGE_TILING_OPTIMAL;
            d.usage  = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            d.flags  = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                      VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            if (withhold) {
                // Withhold the CREATE FLAGS, which is this arm's own
                // declaration. STORAGE goes with them, because a 3-plane
                // 420 image with STORAGE and no MUTABLE|EXTENDED is
                // VK_ERROR_FORMAT_NOT_SUPPORTED on this device -- that is
                // proof 4 restated, and creating it anyway makes the control
                // emit VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251
                // (this driver creates it regardless, which is its own
                // finding). The registration is still refused
                // CONVERSION_REQUIRED, and now for a reason the descriptor
                // states rather than one the create smuggled in.
                d.flags = 0;
                d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            break;
        case ARM_FILTER_RGBA:
            d.tiling = VK_IMAGE_TILING_OPTIMAL;
            // STORAGE and nothing else is what this arm needs. NO create
            // flags: asserting that is half the point of the row.
            d.usage  = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (withhold) {  // the storage read the filter's RGBA arm binds
                d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            break;
        case ARM_CONTROL:
        default:
            d.tiling = VK_IMAGE_TILING_OPTIMAL;
            d.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            break;
    }
    return d;
}

void FillDescriptor(VkVideoEncoderExternalImageDescriptor* desc,
                    VkFormat format, const Decl& d, VkImage image)
{
    std::memset(desc, 0, sizeof(*desc));
    desc->sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc->handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc->format     = format;
    desc->width      = kWidth;
    desc->height     = kHeight;
    desc->tiling     = d.tiling;
    desc->imageUsage = d.usage;
    desc->imageFlags = d.flags;
    desc->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    desc->planeCount  = 0;
    desc->residency   = VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc->defaultLayout = VK_IMAGE_LAYOUT_GENERAL;
    desc->existingImage = image;
}

RowResult RunRow(const Row& row, bool verbose)
{
    RowResult res;

    VkSharedBaseObj<VulkanVideoEncoderExt> enc;
    if ((CreateVulkanVideoEncoderExt(enc) != VK_SUCCESS) || !enc) {
        return res;
    }

    VkVideoEncoderConfig config = {};
    config.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config.codec           = row.codec;
    config.encodeWidth     = kWidth;
    config.encodeHeight    = kHeight;
    config.inputFormat     = row.format;
    config.inputWidth      = kWidth;
    config.inputHeight     = kHeight;
    config.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config.averageBitrate  = 5000000;
    config.maxBitrate      = 5000000;
    config.gopLength       = 30;
    config.consecutiveBFrames = 0;
    config.idrPeriod       = 30;
    config.frameRateNum    = 30;
    config.frameRateDen    = 1;
    config.deviceId        = -1;
    config.disableFileOutput = VK_TRUE;
    // No filter request: the library derives the preprocess conversion from
    // inputFormat, so each row's session is configured exactly as a shipping
    // caller would configure it for that format.

    if (enc->InitializeExt(config) != VK_SUCCESS) {
        return res;
    }
    res.sessionInit    = true;
    res.supportsFormat = (enc->SupportsFormat(row.format) == VK_TRUE);

    VkInstance       instance = enc->GetVkInstance();
    VkDevice         device   = enc->GetVkDevice();
    VkPhysicalDevice phys     = enc->GetVkPhysicalDevice();
    DeviceFns fns;
    if (!LoadDeviceFns(instance, device, &fns)) {
        return res;
    }

    // Device format features, reported for BOTH tilings. This is the raw
    // material filterCapable is derived from, printed beside it so a VK_FALSE
    // can be attributed to the device rather than to the library.
    VkFormatProperties fp{};
    fns.GetPhysicalDeviceFormatProperties(phys, row.format, &fp);
    res.filterCapableOptimal =
        ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0)
            ? VK_TRUE : VK_FALSE;
    res.filterCapableLinear =
        ((fp.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0)
            ? VK_TRUE : VK_FALSE;
    if (verbose) {
        std::printf("      optimalTilingFeatures 0x%08X  "
                    "linearTilingFeatures 0x%08X\n",
                    (unsigned)fp.optimalTilingFeatures,
                    (unsigned)fp.linearTilingFeatures);
    }

    const VkVideoComponentBitDepthFlagBitsKHR depth = GroupDepth(row.group);
    const VkVideoChromaSubsamplingFlagBitsKHR subsampling =
        GroupSubsampling(row.group);
    const Decl d = DeclFor(row.arm, false);
    Img img;
    if (!CreateImage(fns, phys, device, row.format, d.tiling, d.usage, d.flags,
                     d.profileList, row.codec, depth, subsampling, &img)) {
        return res;
    }
    res.imageCreated = true;

    VkVideoEncoderExternalImageDescriptor desc;
    FillDescriptor(&desc, row.format, d, img.image);

    VkVideoEncoderImageSupport support = {};
    support.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT;
    enc->QueryImageSupport(desc, &support);
    res.queryStatus    = support.status;
    res.querySupported = support.supported;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    res.regStatus = enc->RegisterImageResource(desc, 0, &resource, nullptr);
    if ((res.regStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS) &&
        (resource != VK_VIDEO_ENCODER_RESOURCE_NULL)) {
        VkEncResourceProbe probe;
        if (VkEncProbeResource(enc.get(), resource, &probe) ==
            VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            res.path              = probe.inputPath;
            res.planeStorageViews = probe.planeStorageViews;
            res.storageReadView   = probe.storageReadView;
        }
        enc->UnregisterImageResource(resource);
    }

    // WHAT THE DESCRIPTOR DECLARES, on the session and the image the row
    // already built. Nothing here creates a Vulkan object: the only variable
    // is VkVideoEncoderExternalImageDescriptor::colorModel.
    if (row.arm == ARM_FILTER_RGBA) {
        res.ranDeclared = true;
        VkVideoEncoderExternalImageDescriptor cdesc;
        VkVideoEncoderResource cres = VK_VIDEO_ENCODER_RESOURCE_NULL;

        // Naming the model the format already carries is legal and changes
        // nothing.
        FillDescriptor(&cdesc, row.format, d, img.image);
        cdesc.colorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
        res.declaredAgreeing =
            enc->RegisterImageResource(cdesc, 0, &cres, nullptr);
        if (cres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            enc->UnregisterImageResource(cres);
            cres = VK_VIDEO_ENCODER_RESOURCE_NULL;
        }

        // Y'CbCr declared over the same enumerant. On R8G8B8A8_UNORM that is
        // the packed 4:4:4 AYUV reading, which is a statement of fact the
        // library resolves; on the other RGBA spellings it is a contradiction
        // the format cannot carry. Either way this session routes R'G'B'
        // through its filter and cannot route what the descriptor declares.
        FillDescriptor(&cdesc, row.format, d, img.image);
        cdesc.colorModel = VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
        res.declaredYcbcr =
            enc->RegisterImageResource(cdesc, 0, &cres, nullptr);
        if (cres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            enc->UnregisterImageResource(cres);
            cres = VK_VIDEO_ENCODER_RESOURCE_NULL;
        }

        // The same two declarations over a descriptor that also states no
        // extent. A descriptor's colour model is a property of the descriptor
        // alone, so it is judged before the geometry is read: the pair below
        // differs only in the declaration and must therefore answer
        // differently.
        FillDescriptor(&cdesc, row.format, d, img.image);
        cdesc.colorModel = VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
        cdesc.width  = 0;
        cdesc.height = 0;
        res.declaredYcbcrNoExtent =
            enc->RegisterImageResource(cdesc, 0, &cres, nullptr);
        if (cres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            enc->UnregisterImageResource(cres);
            cres = VK_VIDEO_ENCODER_RESOURCE_NULL;
        }

        FillDescriptor(&cdesc, row.format, d, img.image);
        cdesc.width  = 0;
        cdesc.height = 0;
        res.fromFormatNoExtent =
            enc->RegisterImageResource(cdesc, 0, &cres, nullptr);
        if (cres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
            enc->UnregisterImageResource(cres);
        }

        // THE SAME DECLARATION ONE LEVEL UP. Reconfigure carries a whole
        // VkVideoEncoderConfig, and the session's input pair is half of what
        // its preprocess filter was built from -- so a call that re-declares
        // the model has to be refused, not answered VK_SUCCESS with the arm
        // the session already holds. Nothing here reaches the device: a
        // rate-control request is state the encoder thread folds in at the
        // next frame, and this test submits none.
        VkVideoEncoderConfig rcfg = config;
        rcfg.averageBitrate = config.averageBitrate + 1000000;
        rcfg.maxBitrate     = rcfg.averageBitrate;
        res.reconfigBitrate = enc->Reconfigure(rcfg);

        // The control on the COMPARISON rather than on the call: the model
        // this format already carries, stated explicitly instead of left to
        // be read off the format. It resolves to what is in force and must
        // still be accepted -- a gate that compared the spelling would
        // refuse it and refuse the caller nothing useful.
        rcfg = config;
        rcfg.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
        res.reconfigAgreeing = enc->Reconfigure(rcfg);

        // The flip. On R8G8B8A8_UNORM this is AYUV, a reading that enumerant
        // really carries and one this session's filter was not built for; on
        // the other RGBA spellings it resolves to nothing at all. Either way
        // it is not the model the session encodes from.
        rcfg = config;
        rcfg.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
        res.reconfigYcbcr = enc->Reconfigure(rcfg);
        res.ranReconfig   = true;
    }

    // The negative control: same format, same session, the arm's own
    // declaration withheld.
    if (row.arm != ARM_CONTROL) {
        const Decl w = DeclFor(row.arm, true);
        Img wimg;
        if (CreateImage(fns, phys, device, row.format, w.tiling, w.usage,
                        w.flags, w.profileList, row.codec, depth,
                        subsampling, &wimg)) {
            VkVideoEncoderExternalImageDescriptor wdesc;
            FillDescriptor(&wdesc, row.format, w, wimg.image);
            VkVideoEncoderResource wres = VK_VIDEO_ENCODER_RESOURCE_NULL;
            res.withheldStatus =
                enc->RegisterImageResource(wdesc, 0, &wres, nullptr);
            res.ranWithheld = true;
            if (wres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
                enc->UnregisterImageResource(wres);
            }
        }
        DestroyImg(fns, device, &wimg);
    }

    DestroyImg(fns, device, &img);
    enc = nullptr;
    return res;
}

}  // namespace

int main(int argc, char** argv)
{
    bool verbose = false;
    // Which profile family this invocation walks. Each row costs a whole
    // encoder session -- its own VkInstance and VkDevice -- and a single
    // process cannot hold an unbounded number of those, so the matrix is
    // split by family and each family gets its own process. Sharing one
    // would not merely be slower: the rows past the limit report "this
    // device has no such encode profile" for a profile the device does
    // support, which is a wrong finding rather than a missing one.
    bool only444 = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--444") == 0) {
            only444 = true;
        }
    }

    std::printf("Encoder-ext INPUT FORMAT MATRIX -- library-owned device\n");
    std::printf("=======================================================\n");

    // One throwaway session purely to name the device the rest of the matrix
    // ran on. A matrix that silently fell through to a software ICD would be
    // meaningless, and this is what makes that visible rather than assumed.
    {
        VkSharedBaseObj<VulkanVideoEncoderExt> probe;
        if ((CreateVulkanVideoEncoderExt(probe) != VK_SUCCESS) || !probe) {
            std::printf("SKIP: CreateVulkanVideoEncoderExt failed\n");
            return 77;
        }
        VkVideoEncoderConfig c = {};
        c.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
        c.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        c.encodeWidth     = kWidth;
        c.encodeHeight    = kHeight;
        c.inputFormat     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        c.inputWidth      = kWidth;
        c.inputHeight     = kHeight;
        c.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
        c.averageBitrate  = 5000000;
        c.maxBitrate      = 5000000;
        c.gopLength       = 30;
        c.idrPeriod       = 30;
        c.frameRateNum    = 30;
        c.frameRateDen    = 1;
        c.deviceId        = -1;
        c.disableFileOutput = VK_TRUE;
        if (probe->InitializeExt(c) != VK_SUCCESS) {
            std::printf("SKIP: InitializeExt failed -- no encode-capable "
                        "Vulkan device on this host\n");
            return 77;
        }
        DeviceFns fns;
        if (LoadDeviceFns(probe->GetVkInstance(), probe->GetVkDevice(),
                          &fns)) {
            VkPhysicalDeviceProperties props{};
            fns.GetPhysicalDeviceProperties(probe->GetVkPhysicalDevice(),
                                            &props);
            std::printf("device: %s\n", props.deviceName);
            std::printf("  vendorID 0x%04X  deviceID 0x%04X\n",
                        props.vendorID, props.deviceID);
            // driverVersion is printed BOTH ways on purpose. NVIDIA packs it
            // as (major<<22)|(minor<<14)|(secondary<<6)|tertiary; the standard
            // VK_API_VERSION macros unpack (22/12/0), so one driver reads as
            // two different version strings depending on who decodes it.
            std::printf("  driverVersion raw 0x%08X"
                        "  -> standard-decode %u.%u.%u"
                        "  -> NVIDIA-decode %u.%02u\n",
                        props.driverVersion,
                        props.driverVersion >> 22,
                        (props.driverVersion >> 12) & 0x3FF,
                        props.driverVersion & 0xFFF,
                        props.driverVersion >> 22,
                        (props.driverVersion >> 14) & 0xFF);
        }
        probe = nullptr;
    }
    std::printf("\n");

    // ---- THE INPUT-COLOUR CHAIN, AT InitializeExt ----
    //
    // WHY IT IS HERE AND NOT IN THE DEVICE-FREE SUITE. Two walks read
    // VkVideoEncoderConfig::pNext: the BINDER's, which the device-free tests
    // drive, and InitializeExt's, which rejects any sType it does not name.
    // The device-free suite cannot reach the second one -- its session is
    // installed already-initialized -- so a chain that the binder consumes
    // perfectly could still be rejected outright before the binder ever sees
    // it, and nothing would say so. These three sessions are the only place
    // that walk is exercised.
    //
    // THE PAIR IS THE POINT. Every binder refusal is
    // VK_ERROR_INITIALIZATION_FAILED, so a refusal on its own attributes
    // nothing. The accepted config and the refused one differ in ONE FIELD.
    {
        struct ChainCase {
            const char* what;
            uint8_t     bitstreamPrimaries;
            uint8_t     inputPrimaries;
            bool        wantSuccess;
        };
        static const ChainCase chainCases[] = {
            // An all-zero chain is "undeclared on every axis" and must behave
            // exactly like no chain at all.
            { "an all-zero input-colour chain initializes", 0u, 0u, true },
            // CONTROL, and it runs before the refusal is read.
            { "input primaries 9 with bitstream primaries 9 initializes",
              9u, 9u, true },
            { "input primaries 1 with bitstream primaries 9 is refused",
              9u, 1u, false },
        };
        for (const ChainCase& cc : chainCases) {
            VkSharedBaseObj<VulkanVideoEncoderExt> enc;
            if ((CreateVulkanVideoEncoderExt(enc) != VK_SUCCESS) || !enc) {
                Check("input-colour", false, cc.what, "create failed");
                continue;
            }
            VkVideoEncoderInputColourInfo ic = {};
            ic.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO;
            ic.inputColourPrimaries = cc.inputPrimaries;
            VkVideoEncoderConfig c = {};
            c.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
            c.pNext           = &ic;
            c.codec           = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
            c.encodeWidth     = kWidth;
            c.encodeHeight    = kHeight;
            c.inputFormat     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
            c.inputWidth      = kWidth;
            c.inputHeight     = kHeight;
            c.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
            c.averageBitrate  = 5000000;
            c.maxBitrate      = 5000000;
            c.gopLength       = 30;
            c.idrPeriod       = 30;
            c.frameRateNum    = 30;
            c.frameRateDen    = 1;
            c.deviceId        = -1;
            c.disableFileOutput = VK_TRUE;
            c.colourPrimaries = cc.bitstreamPrimaries;
            const VkResult r = enc->InitializeExt(c);
            Check("input-colour",
                  cc.wantSuccess ? (r == VK_SUCCESS)
                                 : (r == VK_ERROR_INITIALIZATION_FAILED),
                  cc.what, "InitializeExt returned " + U32((uint32_t)r));
            enc = nullptr;
        }
    }
    std::printf("\n");

    // The rows this invocation walks. The controls belong to every family:
    // a refusal only ever observed beside 4:2:0 rows says nothing about the
    // engine the 4:4:4 rows actually ran on.
    size_t selected[kNumRows];
    size_t nSelected = 0;
    for (size_t i = 0; i < kNumRows; i++) {
        const bool is444 =
            (kRows[i].group == G_444_8) || (kRows[i].group == G_444_10);
        if ((is444 == only444) || (kRows[i].arm == ARM_CONTROL)) {
            selected[nSelected++] = i;
        }
    }
    std::printf("walking the %s rows: %zu of %zu\n",
                only444 ? "4:4:4" : "4:2:0", nSelected, kNumRows);

    size_t nSessions = 0, nSupports = 0, nRegistered = 0, nFilter = 0;
    size_t nDirect = 0, nStaged = 0, nDeviceLimited = 0;

    // PASS 1 -- run every row. A group's DIRECT control has to be known
    // before any of its siblings can be judged.
    RowResult results[kNumRows];
    bool groupHasDevice[G_NONE + 1] = {false, false, false,
                                       false, false, true};
    for (size_t s = 0; s < nSelected; s++) {
        const size_t i = selected[s];
        results[i] = RunRow(kRows[i], verbose);
        if ((kRows[i].arm == ARM_DIRECT) && results[i].sessionInit) {
            groupHasDevice[kRows[i].group] = true;
        }
    }

    // PASS 2 -- report and assert.
    for (size_t s = 0; s < nSelected; s++) {
        const size_t i = selected[s];
        const Row& row = kRows[i];
        const VkEncInputFormatClass cls =
            VkEncClassifyInput(row.format,
                               VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT);
        const bool deviceHasProfile = groupHasDevice[row.group];
        std::printf("[%zu/%zu] %s   [%s]\n", s + 1, nSelected, row.name,
                    row.codecName);
        std::printf("      taxonomy=%s  planeCount=%u  isRgba=%u\n",
                    ClassName(cls), VkEncInputFormatPlaneCount(row.format),
                    (unsigned)(VkEncResolveColorModel(
                                   row.format,
                                   VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT) ==
                               VK_VIDEO_ENCODER_COLOR_MODEL_RGB));

        const RowResult r = results[i];

        std::printf("      session=%d  SupportsFormat=%d  image=%d\n",
                    (int)r.sessionInit, (int)r.supportsFormat,
                    (int)r.imageCreated);
        std::printf("      STORAGE_IMAGE feature: optimal=%d linear=%d\n",
                    (int)r.filterCapableOptimal, (int)r.filterCapableLinear);
        std::printf("      query: supported=%d status=%d | "
                    "register: status=%d\n",
                    (int)r.querySupported, (int)r.queryStatus,
                    (int)r.regStatus);
        std::printf("      ROUTED: %s   planeStorageViews=%d "
                    "storageReadView=%d\n",
                    PathName(r.path), (int)r.planeStorageViews,
                    (int)r.storageReadView);
        if (r.ranWithheld) {
            std::printf("      negative control (declaration withheld): "
                        "status=%d\n", (int)r.withheldStatus);
        }
        if (r.ranDeclared) {
            std::printf("      colorModel declared: RGB=%d YCBCR=%d ; "
                        "no extent: YCBCR=%d FROM_FORMAT=%d\n",
                        (int)r.declaredAgreeing, (int)r.declaredYcbcr,
                        (int)r.declaredYcbcrNoExtent,
                        (int)r.fromFormatNoExtent);
        }
        if (r.ranReconfig) {
            std::printf("      Reconfigure: bitrate=%d RGB=%d YCBCR=%d\n",
                        (int)r.reconfigBitrate, (int)r.reconfigAgreeing,
                        (int)r.reconfigYcbcr);
        }

        if (r.sessionInit)    nSessions++;
        if (r.supportsFormat) nSupports++;
        if (r.regStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            nRegistered++;
            if (r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER) nFilter++;
            if (r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT) nDirect++;
            if (r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED) nStaged++;
        }

        // ------------------------------------------------------------------
        // DEVICE-LIMITED rows short-circuit. This is a RESULT, not a
        // failure: the group's DIRECT control could not initialize either,
        // so this device has no such video profile and the library was never
        // consulted. The one thing still assertable -- and it is a real
        // assertion, it fails if the library refuses a FILTER format on a
        // profile whose DIRECT sibling works -- is that the two AGREE.
        // ------------------------------------------------------------------
        // The FORMAT TAXONOMY is a pure function of the format: no device,
        // no session, no profile takes part in it. So it is asserted for
        // every row before anything can short-circuit -- otherwise a row
        // whose class regressed would be reported as "this device has no
        // such profile", which is a different finding and a false one.
        if (row.arm == ARM_DIRECT) {
            Check(row.name, cls == VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
                  "taxonomy says DIRECT", ClassName(cls));
        } else if ((row.arm == ARM_FILTER_YCBCR) ||
                   (row.arm == ARM_FILTER_RGBA)) {
            Check(row.name, cls == VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
                  "taxonomy says VIA_FILTER", ClassName(cls));
        }

        if ((row.arm != ARM_CONTROL) && !deviceHasProfile) {
            nDeviceLimited++;
            std::printf("      DEVICE-LIMITED: this device has no encode "
                        "profile at this bit depth; the group's DIRECT "
                        "control did not initialize either.\n");
            Check(row.name, !r.sessionInit,
                  "and the FILTER member agrees with its DIRECT control "
                  "(no library-side refusal hiding behind a device limit)",
                  "sessionInit " + U32(r.sessionInit ? 1 : 0));
            std::printf("\n");
            continue;
        }

        // Whether a Y'CbCr declaration can be READ against this row's
        // format. The three packed 4:4:4 layouts have no Vulkan format of
        // their own and ride RGBA enumerants, so on those the declaration
        // resolves; on every other RGBA spelling it contradicts the format.
        // Asking the library rather than listing the formats is what keeps
        // this from becoming a second copy of that table.
        const bool ycbcrDeclarable =
            (VkEncResolveColorModel(row.format,
                                    VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR) ==
             VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR);
        const VkVideoEncoderStatusCode expectDeclaredYcbcr =
            ycbcrDeclarable
                ? VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED
                : VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED;

        // ------------------------------------------------------------------
        // Assertions. Per arm, because the arms make different promises.
        // ------------------------------------------------------------------
        switch (row.arm) {
            case ARM_DIRECT:
                Check(row.name, r.sessionInit, "session initializes", "");
                Check(row.name,
                      r.regStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS,
                      "registers", "status " + U32(r.regStatus));
                Check(row.name,
                      r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT,
                      "routes DIRECT", PathName(r.path));
                Check(row.name,
                      r.ranWithheld &&
                          (r.withheldStatus ==
                           VK_VIDEO_ENCODER_STATUS_SUCCESS),
                      "and without VIDEO_ENCODE_SRC still registers "
                      "(it stages)",
                      "status " + U32(r.withheldStatus));
                break;
            case ARM_FILTER_YCBCR:
                Check(row.name, r.sessionInit, "session initializes", "");
                Check(row.name,
                      r.regStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS,
                      "registers with MUTABLE|EXTENDED|STORAGE",
                      "status " + U32(r.regStatus));
                Check(row.name,
                      r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER,
                      "routes FILTER", PathName(r.path));
                Check(row.name, r.planeStorageViews == VK_TRUE,
                      "through PLANE STORAGE views (unchanged arm)",
                      "planeStorageViews " + U32(r.planeStorageViews));
                Check(row.name, r.storageReadView == VK_FALSE,
                      "and NOT through the single-plane fact",
                      "storageReadView " + U32(r.storageReadView));
                Check(row.name,
                      r.ranWithheld &&
                          (r.withheldStatus ==
                           VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED),
                      "without the create flags still CONVERSION_REQUIRED",
                      "status " + U32(r.withheldStatus));
                break;
            case ARM_FILTER_RGBA:
                Check(row.name, r.sessionInit, "session initializes", "");
                Check(row.name, r.supportsFormat,
                      "SupportsFormat is VK_TRUE", "");
                Check(row.name,
                      r.regStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS,
                      "registers with STORAGE and NO create flags",
                      "status " + U32(r.regStatus));
                Check(row.name,
                      r.path == VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER,
                      "routes FILTER (from the slot, not the config flag)",
                      PathName(r.path));
                Check(row.name, r.storageReadView == VK_TRUE,
                      "through the SINGLE-PLANE storage-read fact",
                      "storageReadView " + U32(r.storageReadView));
                Check(row.name, r.planeStorageViews == VK_FALSE,
                      "and NOT through plane storage views",
                      "planeStorageViews " + U32(r.planeStorageViews));
                Check(row.name,
                      r.ranWithheld &&
                          (r.withheldStatus ==
                           VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED),
                      "without STORAGE still CONVERSION_REQUIRED "
                      "(the gate still discriminates)",
                      "status " + U32(r.withheldStatus));
                // WHAT THE DESCRIPTOR DECLARES IS WHAT IT IS JUDGED ON.
                // The registration gate is a negotiation point: it answers
                // for the descriptor in front of it, and the colour model is
                // part of that descriptor. Reading the model off the SESSION
                // instead answers about a different picture -- and the packed
                // 4:4:4 layouts are why it matters, because they ride these
                // very enumerants and the format alone cannot tell one of
                // them from an ordinary R'G'B' image.
                Check(row.name,
                      r.ranDeclared &&
                          (r.declaredAgreeing ==
                           VK_VIDEO_ENCODER_STATUS_SUCCESS),
                      "declaring the model the format carries changes nothing",
                      "status " + U32(r.declaredAgreeing));
                // A Y'CbCr declaration over an RGBA enumerant is one of two
                // different things, and the library answers each as itself.
                // Where the enumerant carries a packed 4:4:4 reading the
                // declaration RESOLVES, and what is left is a routing answer:
                // this session converts R'G'B' and cannot route what the
                // descriptor declares. Where it carries none the declaration
                // cannot be read against the format at all, and that is a
                // COLOUR MODEL answer, not a routing one.
                Check(row.name,
                      r.ranDeclared && (r.declaredYcbcr == expectDeclaredYcbcr),
                      ycbcrDeclarable
                          ? "a resolvable Y'CbCr declaration this session "
                            "cannot route is answered on its routing"
                          : "an unresolvable Y'CbCr declaration is answered "
                            "on the declaration itself",
                      "status " + U32(r.declaredYcbcr));
                // The pair that differs ONLY in the declaration. A gate that
                // discards the declared model judges both of these on the
                // R'G'B' route, passes the class question on both, and
                // answers both with the geometry -- which is what makes this
                // the calibration rather than a second positive.
                Check(row.name,
                      r.ranDeclared &&
                          (r.fromFormatNoExtent ==
                           VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID),
                      "an extentless descriptor that declares nothing is "
                      "answered on its geometry",
                      "status " + U32(r.fromFormatNoExtent));
                Check(row.name,
                      r.ranDeclared &&
                          (r.declaredYcbcrNoExtent == expectDeclaredYcbcr),
                      "and the same descriptor declaring Y'CbCr is answered "
                      "on its declaration, before its geometry is read",
                      "status " + U32(r.declaredYcbcrNoExtent));
                // AND THE SESSION IS DECLARED IN THE SAME PAIR. Reconfigure
                // forwards a rate-control change and refuses everything the
                // session was built around; the input colour model is half
                // of what the preprocess filter was built from, so it
                // belongs to the second set. The two positives below are
                // what keep this from passing on a Reconfigure that refuses
                // everything.
                Check(row.name,
                      r.ranReconfig && (r.reconfigBitrate == VK_SUCCESS),
                      "Reconfigure still carries a rate-control change",
                      "VkResult " + std::to_string((int)r.reconfigBitrate));
                Check(row.name,
                      r.ranReconfig && (r.reconfigAgreeing == VK_SUCCESS),
                      "and accepts the model the format already carries "
                      "when it is stated rather than inferred",
                      "VkResult " + std::to_string((int)r.reconfigAgreeing));
                Check(row.name,
                      r.ranReconfig &&
                          (r.reconfigYcbcr == VK_ERROR_INITIALIZATION_FAILED),
                      "and REFUSES a declaration that resolves to the other "
                      "model instead of answering VK_SUCCESS and keeping "
                      "the arm it was built with",
                      "VkResult " + std::to_string((int)r.reconfigYcbcr));
                break;
            case ARM_CONTROL:
            default:
                Check(row.name, cls == VK_ENC_INPUT_FORMAT_UNSUPPORTED,
                      "taxonomy says UNSUPPORTED", ClassName(cls));
                Check(row.name, !r.sessionInit || !r.supportsFormat,
                      "and the session does not claim it", "");
                Check(row.name,
                      r.regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS,
                      "and it does not register",
                      "status " + U32(r.regStatus));
                break;
        }
        std::printf("\n");
    }

    std::printf("=======================================================\n");
    std::printf("DENOMINATOR: %zu formats walked (%zu claimed by the "
                "taxonomy + 2 controls)\n", nSelected, nSelected - 2);
    std::printf("  sessions initialized : %zu of %zu\n", nSessions, nSelected);
    std::printf("  SupportsFormat TRUE  : %zu of %zu\n", nSupports, nSelected);
    std::printf("  registered           : %zu of %zu\n", nRegistered, nSelected);
    std::printf("  routed DIRECT/FILTER/STAGED : %zu / %zu / %zu\n",
                nDirect, nFilter, nStaged);
    std::printf("  device-limited (no such encode profile HERE) : %zu\n",
                nDeviceLimited);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
