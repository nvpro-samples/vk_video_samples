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

#include "VkCodecUtils/Helpers.h"
#include "ShellWin32.h"
#include "FrameProcessor.h"

namespace {

class Win32Timer {
   public:
    Win32Timer() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        freq_ = static_cast<double>(freq.QuadPart);

        reset();
    }

    void reset() { QueryPerformanceCounter(&start_); }

    double get() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        return static_cast<double>(now.QuadPart - start_.QuadPart) / freq_;
    }

   private:
    double freq_;
    LARGE_INTEGER start_;
};

}  // namespace

ShellWin32::ShellWin32(FrameProcessor &frameProcessor, uint32_t deviceID) : Shell(frameProcessor), hwnd_(nullptr) {
    if (frameProcessor.settings().validate) instance_layers_.push_back("VK_LAYER_LUNARG_standard_validation");
    instance_extensions_.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    init_vk(deviceID);
}

ShellWin32::~ShellWin32() {
    cleanup_vk();
    FreeLibrary(hmodule_);
}

void ShellWin32::create_window() {
    const std::string class_name(settings_.name + "WindowClass");

    hinstance_ = GetModuleHandle(nullptr);

    WNDCLASSEX win_class = {};
    win_class.cbSize = sizeof(WNDCLASSEX);
    win_class.style = CS_HREDRAW | CS_VREDRAW;
    win_class.lpfnWndProc = window_proc;
    win_class.hInstance = hinstance_;
    win_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    win_class.lpszClassName = class_name.c_str();
    RegisterClassEx(&win_class);

    const DWORD win_style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE | WS_OVERLAPPEDWINDOW;

    RECT win_rect = {0, 0, settings_.initial_width, settings_.initial_height};
    AdjustWindowRect(&win_rect, win_style, false);

    hwnd_ = CreateWindowEx(WS_EX_APPWINDOW, class_name.c_str(), settings_.name.c_str(), win_style, 0, 0,
                           win_rect.right - win_rect.left, win_rect.bottom - win_rect.top, nullptr, nullptr, hinstance_, nullptr);

    SetForegroundWindow(hwnd_);
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
}

PFN_vkGetInstanceProcAddr ShellWin32::load_vk() {
    const char filename[] = "vulkan-1.dll";
    HMODULE mod;
    PFN_vkGetInstanceProcAddr get_proc = NULL;

    mod = LoadLibrary(filename);
    if (mod) {
        get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"));
    }

    if (!mod || !get_proc) {
        std::stringstream ss;
        ss << "failed to load " << filename;

        if (mod) FreeLibrary(mod);

        throw std::runtime_error(ss.str());
    }

    hmodule_ = mod;

    return get_proc;
}

bool ShellWin32::can_present(VkPhysicalDevice phy, uint32_t queue_family) {
    return vk::GetPhysicalDeviceWin32PresentationSupportKHR(phy, queue_family) == VK_TRUE;
}

VkSurfaceKHR ShellWin32::create_surface(VkInstance instance) {
    VkWin32SurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = hinstance_;
    surface_info.hwnd = hwnd_;

    VkSurfaceKHR surface;
    vk::assert_success(vk::CreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface));

    return surface;
}

LRESULT ShellWin32::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_SIZE: {
            UINT w = LOWORD(lparam);
            UINT h = HIWORD(lparam);
            resize_swapchain(w, h);
        } break;
        case WM_KEYDOWN: {
            FrameProcessor::Key key;

            switch (wparam) {
                case VK_ESCAPE:
                    key = FrameProcessor::KEY_ESC;
                    break;
                case VK_UP:
                    key = FrameProcessor::KEY_UP;
                    break;
                case VK_DOWN:
                    key = FrameProcessor::KEY_DOWN;
                    break;
                case VK_SPACE:
                    key = FrameProcessor::KEY_SPACE;
                    break;
                default:
                    key = FrameProcessor::KEY_UNKNOWN;
                    break;
            }

            frameProcessor_.on_key(key);
        } break;
        case WM_CLOSE:
            frameProcessor_.on_key(FrameProcessor::KEY_SHUTDOWN);
            break;
        case WM_DESTROY:
            quit();
            break;
        default:
            return DefWindowProc(hwnd_, msg, wparam, lparam);
            break;
    }

    return 0;
}

void ShellWin32::quit() { PostQuitMessage(0); }

void ShellWin32::run() {
    create_window();

    create_context();
    resize_swapchain(settings_.initial_width, settings_.initial_height);

    Win32Timer timer;
    double current_time = timer.get();

    while (true) {
        bool quit = false;

        // process all messages
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (quit) break;

        acquire_back_buffer();

        double t = timer.get();
        add_frameProcessor_time(static_cast<float>(t - current_time));

        present_back_buffer();

        current_time = t;
    }

    destroy_context();

    DestroyWindow(hwnd_);
}
