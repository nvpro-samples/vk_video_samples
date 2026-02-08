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

#include "DrmFormats.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace drm_format_mod_test {

//=============================================================================
// Format Mapping Table
//=============================================================================

static const VkToDrmFormatEntry s_formatMappingTable[] = {
    // Single channel 8-bit
    { VK_FORMAT_R8_UNORM, DRM_FORMAT_R8, "VK_FORMAT_R8_UNORM", "DRM_FORMAT_R8" },
    
    // Single channel 16-bit
    { VK_FORMAT_R16_UNORM, DRM_FORMAT_R16, "VK_FORMAT_R16_UNORM", "DRM_FORMAT_R16" },
    
    // Two channel 8-bit
    { VK_FORMAT_R8G8_UNORM, DRM_FORMAT_RG88, "VK_FORMAT_R8G8_UNORM", "DRM_FORMAT_RG88" },
    
    // Two channel 16-bit
    { VK_FORMAT_R16G16_UNORM, DRM_FORMAT_RG1616, "VK_FORMAT_R16G16_UNORM", "DRM_FORMAT_RG1616" },
    
    // RGBA 8-bit
    { VK_FORMAT_R8G8B8A8_UNORM, DRM_FORMAT_ABGR8888, "VK_FORMAT_R8G8B8A8_UNORM", "DRM_FORMAT_ABGR8888" },
    { VK_FORMAT_B8G8R8A8_UNORM, DRM_FORMAT_ARGB8888, "VK_FORMAT_B8G8R8A8_UNORM", "DRM_FORMAT_ARGB8888" },
    { VK_FORMAT_A8B8G8R8_UNORM_PACK32, DRM_FORMAT_RGBA8888, "VK_FORMAT_A8B8G8R8_UNORM_PACK32", "DRM_FORMAT_RGBA8888" },
    
    // sRGB variants
    { VK_FORMAT_R8G8B8A8_SRGB, DRM_FORMAT_ABGR8888, "VK_FORMAT_R8G8B8A8_SRGB", "DRM_FORMAT_ABGR8888" },
    { VK_FORMAT_B8G8R8A8_SRGB, DRM_FORMAT_ARGB8888, "VK_FORMAT_B8G8R8A8_SRGB", "DRM_FORMAT_ARGB8888" },
    
    // 10-bit packed
    { VK_FORMAT_A2R10G10B10_UNORM_PACK32, DRM_FORMAT_ARGB2101010, "VK_FORMAT_A2R10G10B10_UNORM_PACK32", "DRM_FORMAT_ARGB2101010" },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, DRM_FORMAT_ABGR2101010, "VK_FORMAT_A2B10G10R10_UNORM_PACK32", "DRM_FORMAT_ABGR2101010" },
    
    // 16-bit per channel
    { VK_FORMAT_R16G16B16A16_UNORM, DRM_FORMAT_ABGR16161616, "VK_FORMAT_R16G16B16A16_UNORM", "DRM_FORMAT_ABGR16161616" },
    { VK_FORMAT_R16G16B16A16_SFLOAT, DRM_FORMAT_ABGR16161616F, "VK_FORMAT_R16G16B16A16_SFLOAT", "DRM_FORMAT_ABGR16161616F" },
    
    // YCbCr 4:2:0 semi-planar 8-bit
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, DRM_FORMAT_NV12, "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM", "DRM_FORMAT_NV12" },
    
    // YCbCr 4:2:0 semi-planar 10-bit
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, DRM_FORMAT_P010, "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16", "DRM_FORMAT_P010" },
    
    // YCbCr 4:2:0 semi-planar 12-bit
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, DRM_FORMAT_P012, "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16", "DRM_FORMAT_P012" },
    
    // YCbCr 4:2:0 semi-planar 16-bit
    { VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, DRM_FORMAT_P016, "VK_FORMAT_G16_B16R16_2PLANE_420_UNORM", "DRM_FORMAT_P016" },
    
    // YCbCr 4:2:2 semi-planar 8-bit
    { VK_FORMAT_G8_B8R8_2PLANE_422_UNORM, DRM_FORMAT_NV16, "VK_FORMAT_G8_B8R8_2PLANE_422_UNORM", "DRM_FORMAT_NV16" },
    
    // YCbCr 4:2:2 semi-planar 10-bit
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16, DRM_FORMAT_P210, "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16", "DRM_FORMAT_P210" },
    
    // YCbCr 4:2:2 packed
    { VK_FORMAT_G8B8G8R8_422_UNORM, DRM_FORMAT_YUYV, "VK_FORMAT_G8B8G8R8_422_UNORM", "DRM_FORMAT_YUYV" },
    { VK_FORMAT_B8G8R8G8_422_UNORM, DRM_FORMAT_UYVY, "VK_FORMAT_B8G8R8G8_422_UNORM", "DRM_FORMAT_UYVY" },
    
    // YCbCr 4:2:2 packed 10-bit
    { VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16, DRM_FORMAT_Y210, "VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16", "DRM_FORMAT_Y210" },
    
    // YCbCr 4:4:4 packed 10-bit
    // Note: Y410 uses A2B10G10R10 with different interpretation
};

static const size_t s_formatMappingTableSize = sizeof(s_formatMappingTable) / sizeof(s_formatMappingTable[0]);

//=============================================================================
// Format Info Tables
//=============================================================================

static const FormatInfo s_rgbFormats[] = {
    // Single channel
    { VK_FORMAT_R8_UNORM, "R8", "DRM_FORMAT_R8", 1, 1, false, 8, 1, 1 },
    { VK_FORMAT_R16_UNORM, "R16", "DRM_FORMAT_R16", 2, 1, false, 16, 1, 1 },
    
    // Two channel
    { VK_FORMAT_R8G8_UNORM, "RG8", "DRM_FORMAT_RG88", 2, 1, false, 8, 1, 1 },
    { VK_FORMAT_R16G16_UNORM, "RG16", "DRM_FORMAT_RG1616", 4, 1, false, 16, 1, 1 },
    
    // Four channel 8-bit
    { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8", "DRM_FORMAT_ABGR8888", 4, 1, false, 8, 1, 1 },
    { VK_FORMAT_B8G8R8A8_UNORM, "BGRA8", "DRM_FORMAT_ARGB8888", 4, 1, false, 8, 1, 1 },
    { VK_FORMAT_R8G8B8A8_SRGB, "RGBA8_SRGB", "DRM_FORMAT_ABGR8888", 4, 1, false, 8, 1, 1 },
    { VK_FORMAT_B8G8R8A8_SRGB, "BGRA8_SRGB", "DRM_FORMAT_ARGB8888", 4, 1, false, 8, 1, 1 },
    
    // 10-bit packed
    { VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10", "DRM_FORMAT_ARGB2101010", 4, 1, false, 10, 1, 1 },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10", "DRM_FORMAT_ABGR2101010", 4, 1, false, 10, 1, 1 },
    
    // 16-bit per channel
    { VK_FORMAT_R16G16B16A16_UNORM, "RGBA16", "DRM_FORMAT_ABGR16161616", 8, 1, false, 16, 1, 1 },
    { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F", "DRM_FORMAT_ABGR16161616F", 8, 1, false, 16, 1, 1 },
};

static const FormatInfo s_ycbcrFormats[] = {
    // 4:2:0 semi-planar (most important for Vulkan Video)
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "NV12", "DRM_FORMAT_NV12", 0, 2, true, 8, 2, 2 },
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, "P010", "DRM_FORMAT_P010", 0, 2, true, 10, 2, 2 },
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, "P012", "DRM_FORMAT_P012", 0, 2, true, 12, 2, 2 },
    { VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, "P016", "DRM_FORMAT_P016", 0, 2, true, 16, 2, 2 },
    
    // 4:2:2 semi-planar
    { VK_FORMAT_G8_B8R8_2PLANE_422_UNORM, "NV16", "DRM_FORMAT_NV16", 0, 2, true, 8, 2, 1 },
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16, "P210", "DRM_FORMAT_P210", 0, 2, true, 10, 2, 1 },
    
    // 4:2:2 packed
    { VK_FORMAT_G8B8G8R8_422_UNORM, "YUYV", "DRM_FORMAT_YUYV", 2, 1, true, 8, 2, 1 },
    { VK_FORMAT_B8G8R8G8_422_UNORM, "UYVY", "DRM_FORMAT_UYVY", 2, 1, true, 8, 2, 1 },
    
    // 4:2:2 packed 10-bit
    { VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16, "Y210", "DRM_FORMAT_Y210", 4, 1, true, 10, 2, 1 },
};

//=============================================================================
// Format Query Functions
//=============================================================================

std::vector<FormatInfo> getRgbFormats() {
    return std::vector<FormatInfo>(
        s_rgbFormats,
        s_rgbFormats + sizeof(s_rgbFormats) / sizeof(s_rgbFormats[0])
    );
}

std::vector<FormatInfo> getYcbcrFormats() {
    return std::vector<FormatInfo>(
        s_ycbcrFormats,
        s_ycbcrFormats + sizeof(s_ycbcrFormats) / sizeof(s_ycbcrFormats[0])
    );
}

std::vector<FormatInfo> getAllFormats() {
    std::vector<FormatInfo> all;
    auto rgb = getRgbFormats();
    auto ycbcr = getYcbcrFormats();
    all.insert(all.end(), rgb.begin(), rgb.end());
    all.insert(all.end(), ycbcr.begin(), ycbcr.end());
    return all;
}

const FormatInfo* getFormatInfo(VkFormat format) {
    // Check RGB formats
    for (size_t i = 0; i < sizeof(s_rgbFormats) / sizeof(s_rgbFormats[0]); ++i) {
        if (s_rgbFormats[i].vkFormat == format) {
            return &s_rgbFormats[i];
        }
    }
    // Check YCbCr formats
    for (size_t i = 0; i < sizeof(s_ycbcrFormats) / sizeof(s_ycbcrFormats[0]); ++i) {
        if (s_ycbcrFormats[i].vkFormat == format) {
            return &s_ycbcrFormats[i];
        }
    }
    return nullptr;
}

const FormatInfo* getFormatByName(const char* name) {
    // Check RGB formats
    for (size_t i = 0; i < sizeof(s_rgbFormats) / sizeof(s_rgbFormats[0]); ++i) {
        if (strcmp(s_rgbFormats[i].name, name) == 0) {
            return &s_rgbFormats[i];
        }
    }
    // Check YCbCr formats
    for (size_t i = 0; i < sizeof(s_ycbcrFormats) / sizeof(s_ycbcrFormats[0]); ++i) {
        if (strcmp(s_ycbcrFormats[i].name, name) == 0) {
            return &s_ycbcrFormats[i];
        }
    }
    return nullptr;
}

//=============================================================================
// Vulkan <-> DRM Format Conversion
//=============================================================================

uint32_t vkFormatToDrmFormat(VkFormat vkFormat) {
    for (size_t i = 0; i < s_formatMappingTableSize; ++i) {
        if (s_formatMappingTable[i].vkFormat == vkFormat) {
            return s_formatMappingTable[i].drmFormat;
        }
    }
    return 0;
}

VkFormat drmFormatToVkFormat(uint32_t drmFormat) {
    for (size_t i = 0; i < s_formatMappingTableSize; ++i) {
        if (s_formatMappingTable[i].drmFormat == drmFormat) {
            return s_formatMappingTable[i].vkFormat;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

const VkToDrmFormatEntry* getFormatMappingTable(size_t* count) {
    if (count) {
        *count = s_formatMappingTableSize;
    }
    return s_formatMappingTable;
}

const char* drmFormatName(uint32_t drmFormat) {
    for (size_t i = 0; i < s_formatMappingTableSize; ++i) {
        if (s_formatMappingTable[i].drmFormat == drmFormat) {
            return s_formatMappingTable[i].drmName;
        }
    }
    return "UNKNOWN";
}

//=============================================================================
// Modifier Utilities
//=============================================================================

const char* modifierVendorName(uint64_t modifier) {
    uint64_t vendor = modifier >> 56;
    switch (vendor) {
        case 0x00: return "NONE";
        case 0x01: return "INTEL";
        case 0x02: return "AMD";
        case 0x03: return "NVIDIA";
        case 0x04: return "SAMSUNG";
        case 0x05: return "QCOM";
        case 0x08: return "ARM";
        default:   return "UNKNOWN";
    }
}

const char* modifierToString(uint64_t modifier, char* buffer, size_t bufferSize) {
    if (modifier == DRM_FORMAT_MOD_LINEAR) {
        snprintf(buffer, bufferSize, "LINEAR (0x0)");
    } else if (modifier == DRM_FORMAT_MOD_INVALID) {
        snprintf(buffer, bufferSize, "INVALID");
    } else {
        uint64_t vendor = modifier >> 56;
        if (vendor == 0x03) {
            // Decode NVIDIA block-linear modifier fields
            uint32_t h = modifier & 0xF;           // bits 3:0 - log2(blockHeight)
            uint32_t k = (modifier >> 12) & 0xFF;  // bits 19:12 - pageKind
            uint32_t c = (modifier >> 23) & 0x7;   // bits 25:23 - compressionType
            snprintf(buffer, bufferSize, "NVIDIA BL h=%u k=0x%02x c=%u (0x%llx)",
                     h, k, c, (unsigned long long)modifier);
        } else {
            snprintf(buffer, bufferSize, "%s (0x%llx)", 
                     modifierVendorName(modifier),
                     (unsigned long long)modifier);
        }
    }
    return buffer;
}

//=============================================================================
// Format Features to String
//=============================================================================

std::string formatFeaturesToString(VkFormatFeatureFlags flags) {
    std::stringstream ss;
    bool first = true;
    
    auto append = [&](const char* name) {
        if (!first) ss << "|";
        ss << name;
        first = false;
    };
    
    if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) append("SAMPLED");
    if (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) append("STORAGE");
    if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) append("COLOR_ATTACH");
    if (flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) append("DEPTH_STENCIL");
    if (flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT) append("BLIT_SRC");
    if (flags & VK_FORMAT_FEATURE_BLIT_DST_BIT) append("BLIT_DST");
    if (flags & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) append("XFER_SRC");
    if (flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) append("XFER_DST");
    if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) append("FILTER_LINEAR");
    if (flags & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) append("COSITED");
    if (flags & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) append("MIDPOINT");
    if (flags & VK_FORMAT_FEATURE_DISJOINT_BIT) append("DISJOINT");
    
    if (first) ss << "NONE";
    
    return ss.str();
}

//=============================================================================
// DrmModifierInfo Methods
//=============================================================================

std::string DrmModifierInfo::modifierToString() const {
    char buffer[64];
    drm_format_mod_test::modifierToString(modifier, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::string DrmModifierInfo::featuresToString() const {
    return formatFeaturesToString(features);
}

} // namespace drm_format_mod_test
