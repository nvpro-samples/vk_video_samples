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

#include <cassert>
#include <dlfcn.h>
#include <sstream>
#include <time.h>

#include "wayland-client-protocol.h"
#include "ShellWayland.h"

#include "VkCodecUtils/Helpers.h"

#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "VkCodecUtils/FrameProcessor.h"

const std::vector<VkExtensionProperties> waylandSurfaceExtensions {
    VkExtensionProperties{ VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_SPEC_VERSION } };

const std::vector<VkExtensionProperties>& ShellWayland::GetRequiredInstanceExtensions()
{
    return waylandSurfaceExtensions;
}

void ShellWayland::handle_ping(void *data, wl_shell_surface *shell_surface, uint32_t serial) {
    wl_shell_surface_pong(shell_surface, serial);
}

void ShellWayland::handle_configure(void *data, wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height) {}

void ShellWayland::handle_popup_done(void *data, wl_shell_surface *shell_surface) {}

const wl_shell_surface_listener ShellWayland::shell_surface_listener = {handle_ping, handle_configure, handle_popup_done};

void ShellWayland::pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                                        wl_fixed_t sx, wl_fixed_t sy) {}

void ShellWayland::pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}

void ShellWayland::pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {}

void ShellWayland::pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button,
                                         uint32_t state) {
    if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        ShellWayland *shell = (ShellWayland *)data;
        wl_shell_surface_move(shell->shell_surface_, shell->m_seat, serial);
    }
}

void ShellWayland::pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {}

const wl_pointer_listener ShellWayland::pointer_listener = {
    pointer_handle_enter, pointer_handle_leave, pointer_handle_motion, pointer_handle_button, pointer_handle_axis,
};

void ShellWayland::keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {}

void ShellWayland::keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                                         struct wl_array *keys) {}

void ShellWayland::keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {}

void ShellWayland::keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
                                       uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_RELEASED) return;
    ShellWayland *shell = (ShellWayland *)data;
    FrameProcessor::Key frameProcessor_key;
    switch (key) {
        case KEY_ESC:  // Escape
#undef KEY_ESC
            frameProcessor_key = FrameProcessor::KEY_ESC;
            break;
        case KEY_UP:  // up arrow key
#undef KEY_UP
            frameProcessor_key = FrameProcessor::KEY_UP;
            break;
        case KEY_DOWN:  // right arrow key
#undef KEY_DOWN
            frameProcessor_key = FrameProcessor::KEY_DOWN;
            break;
        case KEY_SPACE:  // space bar
#undef KEY_SPACE
            frameProcessor_key = FrameProcessor::KEY_SPACE;
            break;
        default:
#undef KEY_UNKNOWN
            frameProcessor_key = FrameProcessor::KEY_UNKNOWN;
            break;
    }
    if (!shell->m_frameProcessor->OnKey(frameProcessor_key)) {
        shell->QuitLoop();
    }
}

void ShellWayland::keyboard_handle_modifiers(void *data, wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed,
                                             uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {}

const wl_keyboard_listener ShellWayland::keyboard_listener = {
    keyboard_handle_keymap, keyboard_handle_enter, keyboard_handle_leave, keyboard_handle_key, keyboard_handle_modifiers,
};

void ShellWayland::seat_handle_capabilities(void *data, wl_seat *seat, uint32_t caps) {
    // Subscribe to pointer events
    ShellWayland *shell = (ShellWayland *)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !shell->m_pointer) {
        shell->m_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(shell->m_pointer, &pointer_listener, shell);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && shell->m_pointer) {
        wl_pointer_destroy(shell->m_pointer);
        shell->m_pointer = NULL;
    }
    // Subscribe to keyboard events
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        shell->m_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(shell->m_keyboard, &keyboard_listener, shell);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        wl_keyboard_destroy(shell->m_keyboard);
        shell->m_keyboard = NULL;
    }
}

const wl_seat_listener ShellWayland::seat_listener = {
    seat_handle_capabilities,
};

void ShellWayland::registry_handle_global(void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    // pickup wayland objects when they appear
    ShellWayland *shell = (ShellWayland *)data;
    if (strcmp(interface, "wl_compositor") == 0) {
        shell->m_compositor = (wl_compositor *)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shell") == 0) {
        shell->m_shell = (wl_shell *)wl_registry_bind(registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        shell->m_seat = (wl_seat *)wl_registry_bind(registry, id, &wl_seat_interface, 1);
        wl_seat_add_listener(shell->m_seat, &seat_listener, shell);
    }
}

void ShellWayland::registry_handle_global_remove(void *data, wl_registry *registry, uint32_t name) {}

const wl_registry_listener ShellWayland::registry_listener = {registry_handle_global, registry_handle_global_remove};

ShellWayland::ShellWayland(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration)
    : Shell(vkDevCtx, configuration) {

    InitConnection();
}

ShellWayland::~ShellWayland() {

    if (m_keyboard) wl_keyboard_destroy(m_keyboard);
    if (m_pointer) wl_pointer_destroy(m_pointer);
    if (m_seat) wl_seat_destroy(m_seat);
    if (shell_surface_) wl_shell_surface_destroy(shell_surface_);
    if (m_surface) wl_surface_destroy(m_surface);
    if (m_shell) wl_shell_destroy(m_shell);
    if (m_compositor) wl_compositor_destroy(m_compositor);
    if (m_registry) wl_registry_destroy(m_registry);
    if (m_display) wl_display_disconnect(m_display);
}

const char* ShellWayland::GetRequiredInstanceExtension()
{
    return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
}

void ShellWayland::InitConnection() {
    try {
        m_display = wl_display_connect(NULL);
        if (!m_display) throw std::runtime_error("failed to connect to the display server");

        m_registry = wl_display_get_registry(m_display);
        if (!m_registry) throw std::runtime_error("failed to get registry");

        wl_registry_add_listener(m_registry, &ShellWayland::registry_listener, this);
        wl_display_roundtrip(m_display);

        if (!m_compositor) throw std::runtime_error("failed to bind compositor");

        if (!m_shell) throw std::runtime_error("failed to bind shell");
    } catch (const std::exception &e) {
        std::cerr << "Could not initialize Wayland: " << e.what() << std::endl;
        exit(-1);
    }
}

void ShellWayland::CreateWindow() {
    m_surface = wl_compositor_create_surface(m_compositor);
    if (!m_surface) throw std::runtime_error("failed to create surface");

    shell_surface_ = wl_shell_get_shell_surface(m_shell, m_surface);
    if (!shell_surface_) throw std::runtime_error("failed to shell_surface");

    wl_shell_surface_add_listener(shell_surface_, &ShellWayland::shell_surface_listener, this);
    // set title
    wl_shell_surface_set_title(shell_surface_, m_settings.m_windowName.c_str());
    wl_shell_surface_set_toplevel(shell_surface_);
}

bool ShellWayland::PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const {
    return m_ctx.devCtx->GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, presentQueueFamily, m_display);
}

VkSurfaceKHR ShellWayland::CreateSurface(VkInstance instance) {
    VkWaylandSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surface_info.display = m_display;
    surface_info.surface = m_surface;

    VkSurfaceKHR surface;
    Shell::AssertSuccess(m_ctx.devCtx->CreateWaylandSurfaceKHR(instance, &surface_info, nullptr, &surface));

    return surface;
}

void ShellWayland::LoopWait() {
    while (true) {
        if (m_quit_loop) break;

        wl_display_dispatch_pending(m_display);

        AcquireBackBuffer();
        PresentBackBuffer();
    }
}

void ShellWayland::LoopPoll() {

    while (true) {
        if (m_quit_loop) break;

        wl_display_dispatch_pending(m_display);

        AcquireBackBuffer();

        PresentBackBuffer();

    }
}

void ShellWayland::RunLoop() {

    CreateWindow();
    CreateContext();
    ResizeSwapchain(m_settings.m_initialWidth, m_settings.m_initialHeight);

    m_quit_loop = false;
    if (true) {
        LoopPoll();
    } else {
        LoopWait();
    }

    DestroyContext();
    DestroyWindow();
}
