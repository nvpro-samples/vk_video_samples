/*
 * Copyright (C) 2016 Google, Inc.
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

#ifndef SHELL_WAYLAND_H
#define SHELL_WAYLAND_H

#include "Shell.h"

class ShellWayland : public Shell {

public:
    ShellWayland(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration);
    virtual ~ShellWayland();

    static const char* GetRequiredInstanceExtension();
    static const std::vector<VkExtensionProperties>& GetRequiredInstanceExtensions();
    virtual bool PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily)  const;
    virtual void RunLoop();
    virtual void QuitLoop() { m_quit_loop = true; }

private:
    void InitConnection();

    void CreateWindow();
    void DestroyWindow()
    {
        // FIXME: implement - destroy the Wayland surface created by CreateWindow()
    };
    virtual VkSurfaceKHR CreateSurface(VkInstance instance);

    void LoopWait();
    void LoopPoll();

    bool              m_quit_loop;
    wl_display       *m_display;
    wl_registry      *m_registry;
    wl_compositor    *m_compositor;
    wl_shell         *m_shell;
    wl_surface       *m_surface;
    wl_shell_surface *shell_surface_;
    wl_seat          *m_seat;
    wl_pointer       *m_pointer;
    wl_keyboard      *m_keyboard;

    static void handle_ping(void *data, wl_shell_surface *shell_surface, uint32_t serial);
    static void handle_configure(void *data, wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height);
    static void handle_popup_done(void *data, wl_shell_surface *shell_surface);
    static const wl_shell_surface_listener shell_surface_listener;
    static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                                     wl_fixed_t sx, wl_fixed_t sy);
    static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
    static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy);
    static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button,
                                      uint32_t state);
    static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
    static const wl_pointer_listener pointer_listener;
    static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size);
    static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                                      struct wl_array *keys);
    static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
    static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
                                    uint32_t state);
    static void keyboard_handle_modifiers(void *data, wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed,
                                          uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
    static const wl_keyboard_listener keyboard_listener;
    static void seat_handle_capabilities(void *data, wl_seat *seat, uint32_t caps);
    static const wl_seat_listener seat_listener;
    static void registry_handle_global(void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version);
    static void registry_handle_global_remove(void *data, wl_registry *registry, uint32_t name);
    static const wl_registry_listener registry_listener;
};

#endif  // SHELL_WAYLAND_H
