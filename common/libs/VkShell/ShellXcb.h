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

#ifndef SHELL_XCB_H
#define SHELL_XCB_H

#include <xcb/xcb.h>
#include "Shell.h"

class ShellXcb : public Shell {

public:
    ShellXcb(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration);
    virtual ~ShellXcb();

    static const char* GetRequiredInstanceExtension();
    static const std::vector<VkExtensionProperties>& GetRequiredInstanceExtensions();
    virtual bool PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const;
    virtual void RunLoop();
    virtual void QuitLoop() { m_quit_loop = true; }

private:
    void InitConnection();
    void CreateWindow();
    void DestroyWindow();
    virtual VkSurfaceKHR CreateSurface(VkInstance instance);

    void HandleEvent(const xcb_generic_event_t *ev);
    void LoopWait();
    void LoopPoll();

    xcb_connection_t *m_connection;
    xcb_screen_t     *m_screen;
    xcb_window_t      m_window;
    uint16_t          m_winWidth;
    uint16_t          m_winHeight;

    xcb_atom_t m_wm_protocols;
    xcb_atom_t m_wm_delete_window;

    bool m_quit_loop;
};

#endif  // SHELL_XCB_H
