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

#include <cassert>
#include <iostream>
#include <sstream>
#include <dlfcn.h>
#include <time.h>

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/FrameProcessor.h"
#include "ShellXcb.h"

namespace {

xcb_intern_atom_cookie_t intern_atom_cookie(xcb_connection_t *c, const std::string &s) {
    return xcb_intern_atom(c, false, s.size(), s.c_str());
}

xcb_atom_t intern_atom(xcb_connection_t *c, xcb_intern_atom_cookie_t cookie) {
    xcb_atom_t atom = XCB_ATOM_NONE;
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, nullptr);
    if (reply) {
        atom = reply->atom;
        free(reply);
    }

    return atom;
}

}  // namespace

static const std::vector<VkExtensionProperties> xcbSurfaceExtensions {
    VkExtensionProperties{ VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_SPEC_VERSION } };

const std::vector<VkExtensionProperties>& ShellXcb::GetRequiredInstanceExtensions()
{
    return xcbSurfaceExtensions;
}

ShellXcb::ShellXcb(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration)
    : Shell(vkDevCtx, configuration)
    , m_connection()
    , m_screen()
    , m_window()
    , m_winWidth()
    , m_winHeight()
    , m_wm_protocols()
    , m_wm_delete_window()
    , m_quit_loop() {

    InitConnection();
}

ShellXcb::~ShellXcb() {

    xcb_disconnect(m_connection);
}

const char* ShellXcb::GetRequiredInstanceExtension()
{
    return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
}

void ShellXcb::InitConnection()
{

    int scr;
    m_connection = xcb_connect(nullptr, &scr);
    if (!m_connection || xcb_connection_has_error(m_connection)) {
        xcb_disconnect(m_connection);
        throw std::runtime_error("failed to connect to the display server");
    }

    const xcb_setup_t *setup = xcb_get_setup(m_connection);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    while (scr-- > 0) {
        xcb_screen_next(&iter);
    }

    m_screen = iter.data;
}

void ShellXcb::CreateWindow() {
    m_window = xcb_generate_id(m_connection);

    uint32_t value_mask, value_list[32];
    value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    value_list[0] = m_screen->black_pixel;
    value_list[1] = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY;

    m_winWidth  = (uint16_t)m_screen->width_in_pixels;
    m_winHeight  = (uint16_t)m_screen->height_in_pixels;

    xcb_create_window(m_connection, XCB_COPY_FROM_PARENT, m_window, m_screen->root, 0, 0, m_winWidth, m_winHeight, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, m_screen->root_visual, value_mask, value_list);

    xcb_intern_atom_cookie_t utf8_string_cookie = intern_atom_cookie(m_connection, "UTF8_STRING");
    xcb_intern_atom_cookie_t _net_wm_name_cookie = intern_atom_cookie(m_connection, "_NET_WM_NAME");
    xcb_intern_atom_cookie_t wm_protocols_cookie = intern_atom_cookie(m_connection, "WM_PROTOCOLS");
    xcb_intern_atom_cookie_t wm_delete_window_cookie = intern_atom_cookie(m_connection, "WM_DELETE_WINDOW");

    // set title
    xcb_atom_t utf8_string = intern_atom(m_connection, utf8_string_cookie);
    xcb_atom_t _net_wm_name = intern_atom(m_connection, _net_wm_name_cookie);
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window, _net_wm_name, utf8_string, 8,
                        m_settings.m_windowName.size(), m_settings.m_windowName.c_str());

    // advertise WM_DELETE_WINDOW
    m_wm_protocols = intern_atom(m_connection, wm_protocols_cookie);
    m_wm_delete_window = intern_atom(m_connection, wm_delete_window_cookie);
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window, m_wm_protocols, XCB_ATOM_ATOM, 32, 1, &m_wm_delete_window);

    xcb_map_window(m_connection, m_window);
    xcb_flush(m_connection);
}

void ShellXcb::DestroyWindow()
{
    xcb_destroy_window(m_connection, m_window);
    xcb_flush(m_connection);
}

bool ShellXcb::PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const {
    return m_ctx.devCtx->GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice,
                                                                    presentQueueFamily,
                                                                    m_connection, m_screen->root_visual);
}

VkSurfaceKHR ShellXcb::CreateSurface(VkInstance) {
    VkXcbSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    surface_info.connection = m_connection;
    surface_info.window = m_window;

    VkSurfaceKHR surface;
    Shell::AssertSuccess(m_ctx.devCtx->CreateXcbSurfaceKHR(m_ctx.devCtx->getInstance(),
                                                            &surface_info, nullptr, &surface));

    return surface;
}

void ShellXcb::HandleEvent(const xcb_generic_event_t *ev) {
    switch (ev->response_type & 0x7f) {
        case XCB_CONFIGURE_NOTIFY: {
            const xcb_configure_notify_event_t *notify = reinterpret_cast<const xcb_configure_notify_event_t *>(ev);
            if (m_settings.m_verbose) {
                std::cout << "Notify display resize " << notify->width << " x " << notify->height << '\n';
            }

            m_winWidth  = notify->width;
            m_winHeight = notify->height;

            ResizeSwapchain(notify->width, notify->height);
        } break;
        case XCB_KEY_PRESS: {
            const xcb_key_press_event_t *press = reinterpret_cast<const xcb_key_press_event_t *>(ev);
            FrameProcessor::Key key;

            // TODO translate xcb_keycode_t
            switch (press->detail) {
                case 9:
                    key = FrameProcessor::KEY_ESC;
                    break;
                case 111:
                    key = FrameProcessor::KEY_UP;
                    break;
                case 116:
                    key = FrameProcessor::KEY_DOWN;
                    break;
                case 65:
                    key = FrameProcessor::KEY_SPACE;
                    break;
                case 113:
                    key = FrameProcessor::KEY_LEFT;
                    break;
                case 114:
                    key = FrameProcessor::KEY_RIGHT;
                    break;
                case 112:
                     key = FrameProcessor::KEY_PAGE_UP;
                     break;
                 case 117:
                     key = FrameProcessor::KEY_PAGE_DOWN;
                     break;
                default:
                    key = FrameProcessor::KEY_UNKNOWN;
                    printf("KB key %d is unknown\n", press->detail);
                    break;
            }

            if (!m_frameProcessor->OnKey(key)) {
                QuitLoop();
            }
        } break;
        case XCB_CLIENT_MESSAGE: {
            const xcb_client_message_event_t *msg = reinterpret_cast<const xcb_client_message_event_t *>(ev);
            if (msg->type == m_wm_protocols && msg->data.data32[0] == m_wm_delete_window) {
                if (!m_frameProcessor->OnKey(FrameProcessor::KEY_SHUTDOWN)) {
                    QuitLoop();
                }
            }
        } break;
        default:
            break;
    }
}

void ShellXcb::LoopWait() {
    while (true) {
        xcb_generic_event_t *ev = xcb_wait_for_event(m_connection);
        if (!ev) continue;

        HandleEvent(ev);
        free(ev);

        if (m_quit_loop) {
            break;
        }

        AcquireBackBuffer();
        PresentBackBuffer();
    }
}

void ShellXcb::LoopPoll() {

    while (true) {
        // handle pending events
        while (true) {
            xcb_generic_event_t *ev = xcb_poll_for_event(m_connection);
            if (!ev) break;

            HandleEvent(ev);
            free(ev);
        }

        if (m_quit_loop) break;

        AcquireBackBuffer();

        PresentBackBuffer();

    }
}

void ShellXcb::RunLoop() {

    CreateWindow();
    CreateContext();
    ResizeSwapchain(m_winWidth, m_winHeight);

    m_quit_loop = false;
    if (true) {
        LoopPoll();
    } else {
        LoopWait();
    }

    DestroyContext();
    DestroyWindow();
}
