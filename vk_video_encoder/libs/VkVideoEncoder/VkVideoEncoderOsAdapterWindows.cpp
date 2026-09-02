/*
 * Copyright 2025 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VkVideoEncoder/VkVideoEncoderOsAdapterLinux.h"
#include "VkCodecUtils/VkEncoderStdioLatch.h"

#if !defined(_WIN32)
#error "VkVideoEncoderOsAdapterWindows.cpp implements the encoder OS seam for Windows only."
#endif

#include <windows.h>
#include <cstdio>
#include <string>

namespace vkenc {

void OsSetCurrentThreadName(const char* name)
{
    // SetThreadDescription takes UTF-16 and is the only naming API that
    // survives into a crash dump; the older exception-based trick is visible
    // to a debugger only while one is attached.
    //
    // Resolved at run time rather than linked: it arrives in Windows 10 1607,
    // and a hard import would stop the library loading on anything earlier
    // for a facility that only decorates a dump.
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetThreadDescriptionFn setThreadDescription = []() {
        HMODULE kernelBase = ::GetModuleHandleW(L"kernelbase.dll");
        return kernelBase ? reinterpret_cast<SetThreadDescriptionFn>(
                                reinterpret_cast<void*>(::GetProcAddress(
                                    kernelBase, "SetThreadDescription")))
                          : nullptr;
    }();

    if (setThreadDescription == nullptr) {
        return;  // Pre-1607: the thread stays unnamed, which is not an error.
    }

    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wide <= 0) {
        return;
    }
    std::wstring wideName(static_cast<size_t>(wide), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName.data(), wide) <= 0) {
        return;
    }
    const HRESULT hr = setThreadDescription(::GetCurrentThread(), wideName.c_str());
    if (FAILED(hr)) {
        VkEncPrintfErr("[VkVideoEncoder] could not name thread \"%s\" (hr 0x%08lx); "
                     "it will appear unnamed in crash dumps\n",
                     name, static_cast<unsigned long>(hr));
    }
}

VkResult OsSelectDrmFormatModifier(const VulkanDeviceContext* /*vkDevCtx*/,
                                   VkFormat /*format*/,
                                   int32_t /*requestedModifierIndex*/,
                                   uint64_t* pSelectedModifier)
{
    // REFUSED, NOT STUBBED TO ZERO. DRM format modifiers are a Linux/DRM
    // concept: there is no Windows equivalent to select, and returning
    // "modifier 0" would read as DRM_FORMAT_MOD_LINEAR -- a specific, wrong
    // answer that an importer would then act on. A caller that asked for a
    // modifier on this platform asked for something that does not exist, and
    // is told so.
    if (pSelectedModifier != nullptr) {
        *pSelectedModifier = 0;
    }
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

}  // namespace vkenc
