/*
* Copyright 2020 NVIDIA Corporation.
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

#ifndef INCLUDE_YCBCRVKINFO_H_
#define INCLUDE_YCBCRVKINFO_H_

#include <string.h>
#include <cassert>
#include <math.h>
#include "ycbcr_utils.h"
#include "vulkan_interfaces.h"

#ifndef VK_MAX_NUM_IMAGE_PLANES_EXT
#define VK_MAX_NUM_IMAGE_PLANES_EXT 4
#endif

// Multi-planar formats info
typedef struct VkMpFormatInfo {
    VkFormat              vkFormat;
    YcbcrPlanesLayoutInfo planesLayout;     // Plane memory layout;
    VkFormat              vkPlaneFormat[VK_MAX_NUM_IMAGE_PLANES_EXT]; // VkFormats for the corresponding plane.
} VkMpFormatInfo;

// How many rows vkMpFormatInfo[] has. A compile-time bound, so an array sized
// from a walk of the table can be sized at compile time too. nvVkFormats.cpp
// static_asserts it against the table itself, which is where the table is
// visible -- so this cannot drift from it without failing the build.
#define YCBCR_VK_FORMAT_INFO_TABLE_SIZE 38

typedef struct VkFormatDesc {
    VkFormat    format;
    uint8_t     numberOfChannels;
    uint8_t     numberOfBytes;
    const char* name;
} VkFormatDesc;

extern const VkFormatDesc* vkFormatLookUp(VkFormat format);

// Packed 4:4:4 Y'CbCr surfaces carried on RGBA-layout format enums.
//
// Vulkan defines no packed 4:4:4 Y'CbCr format, so the DXGI convention is used: AYUV
// rides on VK_FORMAT_R8G8B8A8_UNORM, Y410 on VK_FORMAT_A2B10G10R10_UNORM_PACK32 and
// Y416 on VK_FORMAT_R16G16B16A16_UNORM. These cannot be added to the multi-planar
// table -- it is densely indexed by (format - VK_FORMAT_G8B8G8R8_422_UNORM), so these
// enums fall outside it, and widening it would reclassify every ordinary RGBA image.
//
// YcbcrVkFormatInfo() therefore returns NULL for them, and every caller that treats
// NULL as "not YCbCr, assume defaults" gets a silently wrong answer -- notably a
// bytes-per-pixel of 1 for what is a 4-byte container.
//
// This is the one description of those aliases. It is only valid where the surface is
// already known to be YCbCr (the filter type or the encoder input config says so); the
// enum alone cannot distinguish AYUV from a genuine RGBA image.
typedef struct VkPackedYcbcrFormatDesc {
    VkFormat    vkFormat;
    uint8_t     bitDepth;          // bits per component actually carried
    uint8_t     bytesPerPixel;     // whole-pixel container size
    uint8_t     yChannel;          // index into (r,g,b,a) carrying Y'
    uint8_t     cbChannel;         // index into (r,g,b,a) carrying Cb
    uint8_t     crChannel;         // index into (r,g,b,a) carrying Cr
    const char* debugName;
} VkPackedYcbcrFormatDesc;

// Returns the packed-4:4:4 description for a format, or NULL if it is not one.
static inline const VkPackedYcbcrFormatDesc* PackedYcbcrFormatDesc(VkFormat format)
{
    static const VkPackedYcbcrFormatDesc packedYcbcrFormatDescs[] = {
        // AYUV: NVENC packs V,U,Y,A into a 32-bit word with V lowest (nvEncodeAPI.h),
        // i.e. memory order V,U,Y,A (ffmpeg "vuya") -> Y=.b Cb=.g Cr=.r
        { VK_FORMAT_R8G8B8A8_UNORM,            8, 4, 2, 1, 0, "AYUV" },
        // Y410: A2B10G10R10 packs A[31:30] B[29:20] G[19:10] R[9:0]; DXGI Y410 stores
        // U[9:0], Y[19:10], V[29:20], A[31:30]  -> Y=.g Cb=.r Cr=.b
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 10, 4, 1, 0, 2, "Y410" },
        // Y416: R16G16B16A16, memory order U,Y,V,A -> Y=.g Cb=.r Cr=.b
        { VK_FORMAT_R16G16B16A16_UNORM,       16, 8, 1, 0, 2, "Y416" },
    };

    for (size_t i = 0;
         i < sizeof(packedYcbcrFormatDescs) / sizeof(packedYcbcrFormatDescs[0]);
         i++) {
        if (packedYcbcrFormatDescs[i].vkFormat == format) {
            return &packedYcbcrFormatDescs[i];
        }
    }
    return NULL;
}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief YcbcrVkFormatInfo returns the ycbcr VkFormat description.
 *
 * @param vkFormat  Vulkan format to be described.
 * @retval pointer to VkMpFormatInfo structure describing the ycbcr format.
 */
const VkMpFormatInfo * YcbcrVkFormatInfo(const VkFormat format);

/**
 * @brief YcbcrVkFormatInfoByIndex WALKS the multi-planar table.
 *
 * YcbcrVkFormatInfo() answers about a format a caller already holds, which is
 * the only question this table could previously be asked. A caller that wants
 * to know WHICH formats it describes -- to derive a routable set, a conversion
 * target or a capability list from the table rather than restate it beside one
 * -- had no way to enumerate it: the table is file-static in ycbcrinfotbl.h,
 * and its two dense VkFormat ranges are spelled only in macros that header
 * does not export.
 *
 * @param index  0 .. YCBCR_VK_FORMAT_INFO_TABLE_SIZE - 1, in table order.
 * @retval pointer to the row, or NULL once |index| is past the end -- so a
 *         walk terminates on the return value and needs no size of its own.
 */
const VkMpFormatInfo * YcbcrVkFormatInfoByIndex(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_YCBCRVKINFO_H_ */
