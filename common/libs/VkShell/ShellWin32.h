/*
 * Copyright (C) 2016 Google, Inc.
 * Copyright 2020 NVIDIA Corporation.
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

#ifndef SHELL_WIN32_H
#define SHELL_WIN32_H

#include <windows.h>
#include "Shell.h"

class ShellWin32 : public Shell {

public:
    ShellWin32(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration);
    virtual ~ShellWin32();

    static const char* GetRequiredInstanceExtension();
    static const std::vector<VkExtensionProperties>& GetRequiredInstanceExtensions();
    virtual bool PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const;
    virtual void RunLoop();
    virtual void QuitLoop();

private:

    void VkCreateWindow();
    void VkDestroyWindow();
    virtual VkSurfaceKHR CreateSurface(VkInstance instance);

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        ShellWin32 *shell = reinterpret_cast<ShellWin32 *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

        // called from constructor, CreateWindowEx specifically.  But why?
        if (!shell) return DefWindowProc(hwnd, uMsg, wParam, lParam);

        return shell->HandleMessage(uMsg, wParam, lParam);
    }
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    HINSTANCE m_hinstance;
    HWND m_hwnd;
};

#endif  // SHELL_WIN32_H
