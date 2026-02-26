/*
 * Copyright 2024 NVIDIA Corporation.
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

#ifndef VK_DRM_FORMAT_MODIFIER_UTILS_H
#define VK_DRM_FORMAT_MODIFIER_UTILS_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#ifdef __linux__
#include "VkCodecUtils/VulkanDeviceContext.h"
#endif

//=============================================================================
// VkDrmFormatModifierUtils
//
// Shared utility for DRM format modifier inspection, querying, and selection.
// Supports NVIDIA, AMD, Intel, and other vendors.
//
// Static methods (modifier decoding, printing) are available on all platforms.
// Instance methods (query, select, dump) require Linux + VulkanDeviceContext.
//=============================================================================

class VkDrmFormatModifierUtils {
public:
    //--- Vendor IDs (upper 8 bits of the 64-bit modifier) ---
    static constexpr uint8_t VENDOR_NONE    = 0x00;
    static constexpr uint8_t VENDOR_INTEL   = 0x01;
    static constexpr uint8_t VENDOR_AMD     = 0x02;
    static constexpr uint8_t VENDOR_NVIDIA  = 0x03;
    static constexpr uint8_t VENDOR_SAMSUNG = 0x04;
    static constexpr uint8_t VENDOR_QCOM    = 0x05;
    static constexpr uint8_t VENDOR_ARM     = 0x08;

    //--- Modifier Decoding (static, vendor-aware, all platforms) ---

    static uint8_t GetVendor(uint64_t modifier) {
        return static_cast<uint8_t>((modifier >> 56) & 0xFF);
    }

    static bool IsLinear(uint64_t modifier) {
        return modifier == 0;
    }

    static bool IsCompressed(uint64_t modifier) {
        if (modifier == 0) return false;
        if (GetVendor(modifier) == VENDOR_NVIDIA) {
            return GetNvCompression(modifier) != 0;
        }
        // Non-NVIDIA: compression is encoded as separate modifier values,
        // not bit-fields. Return false (safe default).
        return false;
    }

    //--- NVIDIA-specific field extraction ---
    // DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h):
    //   val = 0x10 | (h & 0xf) | ((k & 0xff) << 12) | ((g & 0x3) << 20)
    //       | ((s & 0x1) << 22) | ((c & 0x7) << 23)
    // modifier = (vendor << 56) | val

    static uint32_t GetNvBlockHeightLog2(uint64_t modifier) {
        return static_cast<uint32_t>(modifier & 0xF);
    }

    static uint32_t GetNvPageKind(uint64_t modifier) {
        return static_cast<uint32_t>((modifier >> 12) & 0xFF);
    }

    static uint32_t GetNvKindGen(uint64_t modifier) {
        return static_cast<uint32_t>((modifier >> 20) & 0x3);
    }

    static uint32_t GetNvSectorLayout(uint64_t modifier) {
        return static_cast<uint32_t>((modifier >> 22) & 0x1);
    }

    static uint32_t GetNvCompression(uint64_t modifier) {
        return static_cast<uint32_t>((modifier >> 23) & 0x7);
    }

    //--- Printing / Debug (static, all platforms) ---

    static void PrintModifierInfo(uint64_t modifier) {
        if (modifier == 0) {
            printf("  LINEAR (0x0)\n");
            return;
        }
        uint8_t vendor = GetVendor(modifier);
        if (vendor == VENDOR_NVIDIA) {
            uint32_t h = GetNvBlockHeightLog2(modifier);
            uint32_t k = GetNvPageKind(modifier);
            uint32_t g = GetNvKindGen(modifier);
            uint32_t s = GetNvSectorLayout(modifier);
            uint32_t c = GetNvCompression(modifier);

            printf("  NVIDIA Block-Linear 0x%llx\n", (unsigned long long)modifier);
            printf("    blockHeight  = %u (log2 GOBs, %u GOBs = %u rows)\n",
                   h, 1u << h, (1u << h) * 8);
            printf("    pageKind     = 0x%02x\n", k);
            printf("    gobGen       = %u (%s)\n", g,
                   g == 0 ? "Fermi-Volta" : g == 2 ? "Turing+" : "other");
            printf("    sectorLayout = %u (%s)\n", s,
                   s ? "Desktop/Xavier+" : "Tegra K1-Parker");
            printf("    compression  = %u (%s)\n", c,
                   c == 0 ? "none" : "compressed");
        } else if (vendor == VENDOR_INTEL) {
            printf("  Intel modifier 0x%llx\n", (unsigned long long)modifier);
        } else if (vendor == VENDOR_AMD) {
            printf("  AMD modifier 0x%llx\n", (unsigned long long)modifier);
        } else {
            printf("  Unknown vendor (0x%02x) modifier 0x%llx\n",
                   vendor, (unsigned long long)modifier);
        }
    }

    static std::string ModifierToString(uint64_t modifier) {
        if (modifier == 0) {
            return "LINEAR (0x0)";
        }
        char buf[128];
        uint8_t vendor = GetVendor(modifier);
        if (vendor == VENDOR_NVIDIA) {
            uint32_t h = GetNvBlockHeightLog2(modifier);
            uint32_t k = GetNvPageKind(modifier);
            uint32_t c = GetNvCompression(modifier);
            snprintf(buf, sizeof(buf),
                     "NVIDIA BL h=%u k=0x%02x c=%u (0x%llX)",
                     h, k, c, (unsigned long long)modifier);
        } else if (vendor == VENDOR_INTEL) {
            snprintf(buf, sizeof(buf), "Intel (0x%llX)",
                     (unsigned long long)modifier);
        } else if (vendor == VENDOR_AMD) {
            snprintf(buf, sizeof(buf), "AMD (0x%llX)",
                     (unsigned long long)modifier);
        } else {
            snprintf(buf, sizeof(buf), "vendor=0x%02x (0x%llX)",
                     vendor, (unsigned long long)modifier);
        }
        return std::string(buf);
    }

#ifdef __linux__
    //=========================================================================
    // Instance methods — Linux only (require VK_EXT_image_drm_format_modifier)
    //=========================================================================

    //--- Querying available modifiers ---

    struct ModifierInfo {
        uint64_t             modifier;
        uint32_t             planeCount;
        VkFormatFeatureFlags features;
    };

    std::vector<ModifierInfo> QueryModifiers(VkFormat format) const {
        std::vector<ModifierInfo> result;

        VkDrmFormatModifierPropertiesListEXT modList{
            VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fmtProps2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        fmtProps2.pNext = &modList;
        queryFormatProperties(format, &fmtProps2);

        if (modList.drmFormatModifierCount == 0) {
            return result;
        }

        std::vector<VkDrmFormatModifierPropertiesEXT> modProps(
            modList.drmFormatModifierCount);
        modList.pDrmFormatModifierProperties = modProps.data();
        queryFormatProperties(format, &fmtProps2);

        result.reserve(modProps.size());
        for (const auto& m : modProps) {
            result.push_back({
                m.drmFormatModifier,
                m.drmFormatModifierPlaneCount,
                m.drmFormatModifierTilingFeatures
            });
        }
        return result;
    }

    //--- Selection ---

    enum class CompressionPref { DontCare, PreferCompressed, PreferUncompressed };
    enum class BlockHeightPref { DontCare, PreferSmallest, PreferLargest };

    // Select a DRM modifier for the given format.
    // Returns the modifier value, or 0 (LINEAR) if no suitable modifier found.
    //
    // explicitIndex >= 0: return modifiers[explicitIndex] unconditionally
    // Otherwise: filter by requiredFeatures, sort by block height / compression prefs.
    uint64_t SelectModifier(
        VkFormat format,
        VkFormatFeatureFlags requiredFeatures,
        int32_t explicitIndex = -1,
        BlockHeightPref blockPref = BlockHeightPref::PreferSmallest,
        CompressionPref compPref = CompressionPref::PreferUncompressed,
        bool allowLinear = false
    ) const {
        auto allMods = QueryModifiers(format);

        if (allMods.empty()) {
            return 0;
        }

        // Explicit index: use unconditionally if valid
        if (explicitIndex >= 0 && (size_t)explicitIndex < allMods.size()) {
            return allMods[explicitIndex].modifier;
        }

        // Build candidate list: non-linear modifiers with required features
        struct Candidate {
            uint64_t modifier;
            uint32_t compression;
            uint32_t blockHeightLog2;
        };
        std::vector<Candidate> candidates;

        for (const auto& m : allMods) {
            if (m.modifier == 0) continue;  // skip LINEAR
            if ((m.features & requiredFeatures) != requiredFeatures) continue;
            candidates.push_back({
                m.modifier,
                GetNvCompression(m.modifier),
                GetNvBlockHeightLog2(m.modifier)
            });
        }

        // Sort by block height preference, then compression preference
        bool prefSmallest = (blockPref == BlockHeightPref::PreferSmallest);
        bool prefCompressed = (compPref == CompressionPref::PreferCompressed);

        std::sort(candidates.begin(), candidates.end(),
            [prefSmallest, prefCompressed](const Candidate& a, const Candidate& b) {
                if (a.blockHeightLog2 != b.blockHeightLog2) {
                    return prefSmallest
                        ? (a.blockHeightLog2 < b.blockHeightLog2)
                        : (a.blockHeightLog2 > b.blockHeightLog2);
                }
                bool aMatch = prefCompressed
                    ? (a.compression > 0) : (a.compression == 0);
                bool bMatch = prefCompressed
                    ? (b.compression > 0) : (b.compression == 0);
                if (aMatch != bMatch) return aMatch;
                return false;
            });

        if (!candidates.empty()) {
            return candidates[0].modifier;
        }

        // Fallback: any modifier with required features (including linear)
        if (allowLinear) {
            for (const auto& m : allMods) {
                if ((m.features & requiredFeatures) == requiredFeatures) {
                    return m.modifier;
                }
            }
        }

        return 0;
    }

    // Dump all available modifiers for a format (for debug logging).
    void DumpAvailableModifiers(VkFormat format,
                                VkFormatFeatureFlags requiredFeatures = 0) const {
        auto allMods = QueryModifiers(format);

        printf("\n=== DRM Format Modifiers for format %d (%zu available) ===\n",
               format, allMods.size());

        for (size_t i = 0; i < allMods.size(); i++) {
            const auto& m = allMods[i];
            bool hasRequired = (requiredFeatures == 0) ||
                ((m.features & requiredFeatures) == requiredFeatures);

            printf("  [%2zu] mod=0x%016llx planes=%u features=0x%08x %s\n",
                   i, (unsigned long long)m.modifier,
                   m.planeCount, m.features,
                   IsLinear(m.modifier) ? "LINEAR" :
                   !hasRequired ? "SKIP(missing features)" : "OK");

            if (!IsLinear(m.modifier) && GetVendor(m.modifier) == VENDOR_NVIDIA) {
                printf("       c=%u h=%u (%u GOBs)\n",
                       GetNvCompression(m.modifier),
                       GetNvBlockHeightLog2(m.modifier),
                       1u << GetNvBlockHeightLog2(m.modifier));
            }
        }
    }

    //--- Construction (Linux only) ---

    VkDrmFormatModifierUtils(const VulkanDeviceContext* vkDevCtx)
        : m_vkDevCtx(vkDevCtx) {}

private:
    const VulkanDeviceContext* m_vkDevCtx;

    void queryFormatProperties(VkFormat format, VkFormatProperties2* pProps) const {
        m_vkDevCtx->GetPhysicalDeviceFormatProperties2(
            m_vkDevCtx->getPhysicalDevice(), format, pProps);
    }
#endif // __linux__
};

#endif // VK_DRM_FORMAT_MODIFIER_UTILS_H
