/*
 * Copyright 2024-2026 NVIDIA Corporation.
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

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>
#include <string>

namespace drm_format_mod_test {

//=============================================================================
// DRM FourCC Format Codes (from drm_fourcc.h)
//=============================================================================

// Helper macro for creating FourCC codes
#define DRM_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

// Single channel formats
static constexpr uint32_t DRM_FORMAT_R8       = DRM_FOURCC('R', '8', ' ', ' ');
static constexpr uint32_t DRM_FORMAT_R16      = DRM_FOURCC('R', '1', '6', ' ');

// Two channel formats
static constexpr uint32_t DRM_FORMAT_RG88     = DRM_FOURCC('R', 'G', '8', '8');
static constexpr uint32_t DRM_FORMAT_GR88     = DRM_FOURCC('G', 'R', '8', '8');
static constexpr uint32_t DRM_FORMAT_RG1616   = DRM_FOURCC('R', 'G', '3', '2');
static constexpr uint32_t DRM_FORMAT_GR1616   = DRM_FOURCC('G', 'R', '3', '2');

// RGB 24-bit formats
static constexpr uint32_t DRM_FORMAT_RGB888   = DRM_FOURCC('R', 'G', '2', '4');
static constexpr uint32_t DRM_FORMAT_BGR888   = DRM_FOURCC('B', 'G', '2', '4');

// RGBA 32-bit formats
static constexpr uint32_t DRM_FORMAT_XRGB8888 = DRM_FOURCC('X', 'R', '2', '4');
static constexpr uint32_t DRM_FORMAT_XBGR8888 = DRM_FOURCC('X', 'B', '2', '4');
static constexpr uint32_t DRM_FORMAT_RGBX8888 = DRM_FOURCC('R', 'X', '2', '4');
static constexpr uint32_t DRM_FORMAT_BGRX8888 = DRM_FOURCC('B', 'X', '2', '4');
static constexpr uint32_t DRM_FORMAT_ARGB8888 = DRM_FOURCC('A', 'R', '2', '4');
static constexpr uint32_t DRM_FORMAT_ABGR8888 = DRM_FOURCC('A', 'B', '2', '4');
static constexpr uint32_t DRM_FORMAT_RGBA8888 = DRM_FOURCC('R', 'A', '2', '4');
static constexpr uint32_t DRM_FORMAT_BGRA8888 = DRM_FOURCC('B', 'A', '2', '4');

// 10-bit packed formats
static constexpr uint32_t DRM_FORMAT_XRGB2101010 = DRM_FOURCC('X', 'R', '3', '0');
static constexpr uint32_t DRM_FORMAT_XBGR2101010 = DRM_FOURCC('X', 'B', '3', '0');
static constexpr uint32_t DRM_FORMAT_ARGB2101010 = DRM_FOURCC('A', 'R', '3', '0');
static constexpr uint32_t DRM_FORMAT_ABGR2101010 = DRM_FOURCC('A', 'B', '3', '0');

// 16-bit per channel formats
static constexpr uint32_t DRM_FORMAT_XRGB16161616 = DRM_FOURCC('X', 'R', '4', '8');
static constexpr uint32_t DRM_FORMAT_XBGR16161616 = DRM_FOURCC('X', 'B', '4', '8');
static constexpr uint32_t DRM_FORMAT_ARGB16161616 = DRM_FOURCC('A', 'R', '4', '8');
static constexpr uint32_t DRM_FORMAT_ABGR16161616 = DRM_FOURCC('A', 'B', '4', '8');

// Float formats
static constexpr uint32_t DRM_FORMAT_XRGB16161616F = DRM_FOURCC('X', 'R', '4', 'H');
static constexpr uint32_t DRM_FORMAT_XBGR16161616F = DRM_FOURCC('X', 'B', '4', 'H');
static constexpr uint32_t DRM_FORMAT_ARGB16161616F = DRM_FOURCC('A', 'R', '4', 'H');
static constexpr uint32_t DRM_FORMAT_ABGR16161616F = DRM_FOURCC('A', 'B', '4', 'H');

//=============================================================================
// YCbCr Formats
//=============================================================================

// 4:2:0 semi-planar (NV12/NV21)
static constexpr uint32_t DRM_FORMAT_NV12     = DRM_FOURCC('N', 'V', '1', '2');
static constexpr uint32_t DRM_FORMAT_NV21     = DRM_FOURCC('N', 'V', '2', '1');

// 4:2:0 semi-planar 10-bit (P010)
static constexpr uint32_t DRM_FORMAT_P010     = DRM_FOURCC('P', '0', '1', '0');
static constexpr uint32_t DRM_FORMAT_P012     = DRM_FOURCC('P', '0', '1', '2');
static constexpr uint32_t DRM_FORMAT_P016     = DRM_FOURCC('P', '0', '1', '6');

// 4:2:2 semi-planar
static constexpr uint32_t DRM_FORMAT_NV16     = DRM_FOURCC('N', 'V', '1', '6');
static constexpr uint32_t DRM_FORMAT_NV61     = DRM_FOURCC('N', 'V', '6', '1');

// 4:2:2 semi-planar 10-bit
static constexpr uint32_t DRM_FORMAT_P210     = DRM_FOURCC('P', '2', '1', '0');

// 4:4:4 planar
static constexpr uint32_t DRM_FORMAT_YUV444   = DRM_FOURCC('Y', 'U', '2', '4');

// Packed YCbCr
static constexpr uint32_t DRM_FORMAT_YUYV     = DRM_FOURCC('Y', 'U', 'Y', 'V');
static constexpr uint32_t DRM_FORMAT_YVYU     = DRM_FOURCC('Y', 'V', 'Y', 'U');
static constexpr uint32_t DRM_FORMAT_UYVY     = DRM_FOURCC('U', 'Y', 'V', 'Y');
static constexpr uint32_t DRM_FORMAT_VYUY     = DRM_FOURCC('V', 'Y', 'U', 'Y');

// 10-bit packed 4:2:2
static constexpr uint32_t DRM_FORMAT_Y210     = DRM_FOURCC('Y', '2', '1', '0');
static constexpr uint32_t DRM_FORMAT_Y212     = DRM_FOURCC('Y', '2', '1', '2');
static constexpr uint32_t DRM_FORMAT_Y216     = DRM_FOURCC('Y', '2', '1', '6');

// 10-bit packed 4:4:4
static constexpr uint32_t DRM_FORMAT_Y410     = DRM_FOURCC('Y', '4', '1', '0');
static constexpr uint32_t DRM_FORMAT_Y412     = DRM_FOURCC('Y', '4', '1', '2');
static constexpr uint32_t DRM_FORMAT_Y416     = DRM_FOURCC('Y', '4', '1', '6');

//=============================================================================
// Format Modifier Constants
//=============================================================================

// Linear modifier (universal)
static constexpr uint64_t DRM_FORMAT_MOD_LINEAR   = 0ULL;
static constexpr uint64_t DRM_FORMAT_MOD_NONE     = 0ULL;  // Alias for LINEAR

// Invalid modifier (sentinel)
static constexpr uint64_t DRM_FORMAT_MOD_INVALID  = ((1ULL << 56) - 1);

// Vendor prefixes (upper 8 bits)
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_NONE     = 0x00ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_INTEL    = 0x01ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_AMD      = 0x02ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_NVIDIA   = 0x03ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_SAMSUNG  = 0x04ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_QCOM     = 0x05ULL << 56;
static constexpr uint64_t DRM_FORMAT_MOD_VENDOR_ARM      = 0x08ULL << 56;

//=============================================================================
// Format Information
//=============================================================================

struct FormatInfo {
    VkFormat        vkFormat;
    const char*     name;
    const char*     drmName;        // DRM fourcc name (if applicable)
    uint32_t        bytesPerPixel;  // For single-plane formats
    uint32_t        planeCount;
    bool            isYcbcr;
    uint32_t        bitDepth;       // Per-component bit depth
    
    // Subsampling info for YCbCr
    uint32_t        chromaSubsampleX;  // 1=4:4:4, 2=4:2:2/4:2:0
    uint32_t        chromaSubsampleY;  // 1=4:4:4/4:2:2, 2=4:2:0
};

// Get all formats to test
std::vector<FormatInfo> getRgbFormats();
std::vector<FormatInfo> getYcbcrFormats();
std::vector<FormatInfo> getAllFormats();

// Get format info by VkFormat
const FormatInfo* getFormatInfo(VkFormat format);

// Get format info by name
const FormatInfo* getFormatByName(const char* name);

//=============================================================================
// DRM Modifier Properties
//=============================================================================

struct DrmModifierInfo {
    uint64_t                modifier;
    uint32_t                planeCount;
    VkFormatFeatureFlags    features;
    VkFormatFeatureFlags2   features2;      // Extended features (VK 1.3+)
    
    bool isLinear() const { return modifier == DRM_FORMAT_MOD_LINEAR; }
    
    // Check if modifier has compression enabled (vendor-aware)
    // NVIDIA: bits 25:23 encode compressionType
    // Intel/AMD: separate modifier values (not detectable from bits alone)
    bool isCompressed() const {
        if (isLinear()) return false;
        uint8_t vendor = static_cast<uint8_t>((modifier >> 56) & 0xFF);
        if (vendor == 0x03) {  // NVIDIA
            return ((modifier >> 23) & 0x7ULL) != 0;
        }
        return false;  // Non-NVIDIA: treat as uncompressed (safe default)
    }
    
    // Get NVIDIA compression type (0=none, 1=ROP/3D layout 1, 2=ROP/3D layout 2, etc.)
    // Only valid for NVIDIA modifiers (vendor prefix 0x03)
    uint32_t compressionType() const {
        if (isLinear()) return 0;
        uint8_t vendor = static_cast<uint8_t>((modifier >> 56) & 0xFF);
        if (vendor != 0x03) return 0;  // Non-NVIDIA
        return static_cast<uint32_t>((modifier >> 23) & 0x7ULL);
    }
    
    std::string modifierToString() const;
    std::string featuresToString() const;
};

// Convert VkFormatFeatureFlags to string
std::string formatFeaturesToString(VkFormatFeatureFlags flags);

//=============================================================================
// Vulkan to DRM Format Mapping
//=============================================================================

struct VkToDrmFormatEntry {
    VkFormat    vkFormat;
    uint32_t    drmFormat;
    const char* vkName;
    const char* drmName;
};

// Get DRM format for a Vulkan format (returns 0 if no mapping)
uint32_t vkFormatToDrmFormat(VkFormat vkFormat);

// Get Vulkan format for a DRM format (returns VK_FORMAT_UNDEFINED if no mapping)
VkFormat drmFormatToVkFormat(uint32_t drmFormat);

// Get format mapping table
const VkToDrmFormatEntry* getFormatMappingTable(size_t* count);

// Get DRM format name
const char* drmFormatName(uint32_t drmFormat);

// Get modifier vendor name
const char* modifierVendorName(uint64_t modifier);

// Format modifier to string (includes vendor prefix)
const char* modifierToString(uint64_t modifier, char* buffer, size_t bufferSize);

} // namespace drm_format_mod_test
