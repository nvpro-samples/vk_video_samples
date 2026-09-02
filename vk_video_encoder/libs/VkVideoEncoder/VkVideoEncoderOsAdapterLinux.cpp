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
 * Linux implementation of the encoder core OS seam declared in
 * VkVideoEncoderOsAdapterLinux.h.
 *
 * This translation unit is platform-specific by construction. Another
 * platform is served by another translation unit implementing the same
 * declarations, selected by the build; it is never served by an #else arm in
 * this one.
 */
#include "VkVideoEncoder/VkVideoEncoderOsAdapterLinux.h"
#include "VkCodecUtils/VkEncoderStdioLatch.h"

#if !defined(__linux__)
#error "VkVideoEncoderOsAdapterLinux.cpp implements the encoder OS seam for Linux only. Port the seam in a sibling translation unit and select it in the build."
#endif

#include <cstdio>

#include <pthread.h>

#include "VkCodecUtils/VkDrmFormatModifierUtils.h"

namespace vkenc {

void OsSetCurrentThreadName(const char* name)
{
    // The result is checked and reported: a thread that silently failed to be
    // named is indistinguishable in a crash dump from one that was never
    // named at all, and the report is what tells those two apart.
    const int rc = pthread_setname_np(pthread_self(), name);
    if (rc != 0) {
        VkEncPrintfErr("[VkVideoEncoder] could not name thread \"%s\" (errno %d); "
                "it will appear unnamed in crash dumps\n",
                name, rc);
    }
}

VkResult OsSelectDrmFormatModifier(const VulkanDeviceContext* vkDevCtx,
                                   VkFormat format,
                                   int32_t requestedModifierIndex,
                                   uint64_t* pSelectedModifier)
{
    VkDrmFormatModifierUtils drmUtils(vkDevCtx);

    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_VIDEO_ENCODE_INPUT_BIT_KHR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    drmUtils.DumpAvailableModifiers(format, required);

    const int32_t idx = requestedModifierIndex;
    uint64_t selected = drmUtils.SelectModifier(
        format, required, idx,
        VkDrmFormatModifierUtils::BlockHeightPref::PreferSmallest,
        VkDrmFormatModifierUtils::CompressionPref::PreferUncompressed);

    if (selected == 0 && idx >= 0) {
        // Explicit index was requested but no suitable modifier found
        VkEncPrintfErr("DRM modifier index %d: no suitable modifier found\n", idx);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (selected == 0) {
        VkEncPrintfErr("No non-linear DRM modifiers support VIDEO_ENCODE_SRC + TRANSFER_DST\n");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    *pSelectedModifier = selected;
    VkEncPrintfOut("\n=== Selected DRM format modifier ===\n");
    VkDrmFormatModifierUtils::PrintModifierInfo(selected);
    VkEncPrintfOut("\n");

    return VK_SUCCESS;
}

}  // namespace vkenc
