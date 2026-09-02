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
 * OS adapter for the encoder core.
 *
 * The encoder core carries no OS-specific code -- no <sys/eventfd.h>, no
 * <windows.h>, no poll/epoll. Everything OS-conditional it needs is declared
 * here in platform-neutral terms, and is the core-layer counterpart of the
 * ext layer's vulkan_video_encoder_os_event_linux seam.
 *
 * These declarations are portable; the implementations are not. Exactly one
 * implementation translation unit is compiled per platform and the build
 * selects it; the Linux one is VkVideoEncoderOsAdapterLinux.cpp. A platform
 * with no implementation stops the build at configure time, so an unported
 * platform is a build error and never a run-time no-op.
 */
#ifndef VK_VIDEO_ENCODER_OS_ADAPTER_LINUX_H_
#define VK_VIDEO_ENCODER_OS_ADAPTER_LINUX_H_

#include <stdint.h>

#include <vulkan/vulkan.h>

class VulkanDeviceContext;

namespace vkenc {

// Give the calling thread a name the OS will report. Linux caps this at
// 15 characters plus NUL and fails silently past it, so keep the names
// short. A named thread is what lets a stack inside the encoder be
// attributed to this library in a crash dump.
void OsSetCurrentThreadName(const char* name);

// Select a DRM format modifier suitable for VIDEO_ENCODE_SRC + TRANSFER_DST
// and write it to *pSelectedModifier. requestedModifierIndex >= 0 demands
// that entry of the reported list and fails with
// VK_ERROR_INITIALIZATION_FAILED if it is not suitable; a negative index
// lets the utility choose, failing with VK_ERROR_FORMAT_NOT_SUPPORTED when
// nothing qualifies.
VkResult OsSelectDrmFormatModifier(const VulkanDeviceContext* vkDevCtx,
                                   VkFormat format,
                                   int32_t requestedModifierIndex,
                                   uint64_t* pSelectedModifier);

}  // namespace vkenc

#endif  // VK_VIDEO_ENCODER_OS_ADAPTER_LINUX_H_
