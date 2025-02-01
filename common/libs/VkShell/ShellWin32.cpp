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
#include "VkCodecUtils/FrameProcessor.h"
#include "ShellWin32.h"

static const std::vector<VkExtensionProperties> win32SurfaceExtensions {
    VkExtensionProperties{ VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION } };

const std::vector<VkExtensionProperties>& ShellWin32::GetRequiredInstanceExtensions()
{
    return win32SurfaceExtensions;
}

ShellWin32::ShellWin32(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration)
    : Shell(vkDevCtx, configuration) {
}

ShellWin32::~ShellWin32() {
}

const char* ShellWin32::GetRequiredInstanceExtension()
{
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

void ShellWin32::VkCreateWindow() {
    const std::string class_name(m_settings.m_windowName + "WindowClass");

    m_hinstance = GetModuleHandle(nullptr);

    WNDCLASSEX win_class = {};
    win_class.cbSize = sizeof(WNDCLASSEX);
    win_class.style = CS_HREDRAW | CS_VREDRAW;
    win_class.lpfnWndProc = window_proc;
    win_class.hInstance = m_hinstance;
    win_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    win_class.lpszClassName = class_name.c_str();
    RegisterClassEx(&win_class);

    const DWORD win_style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE | WS_OVERLAPPEDWINDOW;

    RECT win_rect = {0, 0, m_settings.m_initialWidth, m_settings.m_initialHeight};
    AdjustWindowRect(&win_rect, win_style, false);

    m_hwnd = CreateWindowEx(WS_EX_APPWINDOW, class_name.c_str(), m_settings.m_windowName.c_str(), win_style, 0, 0,
                            win_rect.right - win_rect.left, win_rect.bottom - win_rect.top,
                            nullptr, nullptr, m_hinstance, nullptr);

    SetForegroundWindow(m_hwnd);
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
}

void ShellWin32::VkDestroyWindow()
{
    ::DestroyWindow(m_hwnd);
}

bool ShellWin32::PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const {
    return m_ctx.devCtx->GetPhysicalDeviceWin32PresentationSupportKHR(physicalDevice, presentQueueFamily) == VK_TRUE;
}

VkSurfaceKHR ShellWin32::CreateSurface(VkInstance instance) {
    VkWin32SurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = m_hinstance;
    surface_info.hwnd = m_hwnd;

    VkSurfaceKHR surface;
    Shell::AssertSuccess(m_ctx.devCtx->CreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface));

    return surface;
}

LRESULT ShellWin32::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_SIZE: {
            UINT w = LOWORD(lparam);
            UINT h = HIWORD(lparam);
            ResizeSwapchain(w, h);
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

            if (!m_frameProcessor->OnKey(key)) {
                QuitLoop();
            }

        } break;
        case WM_CLOSE:
            if (!m_frameProcessor->OnKey(FrameProcessor::KEY_SHUTDOWN)) {
                QuitLoop();
            }
            break;
        case WM_DESTROY:
            QuitLoop();
            break;
        default:
            return DefWindowProc(m_hwnd, msg, wparam, lparam);
            break;
    }

    return 0;
}

void ShellWin32::QuitLoop() { PostQuitMessage(0); }

void ShellWin32::RunLoop() {

    VkCreateWindow();
    CreateContext();
    ResizeSwapchain(m_settings.m_initialWidth, m_settings.m_initialHeight);

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

        if (quit) {
            break;
        }

        AcquireBackBuffer();
        PresentBackBuffer();
    }

    DestroyContext();
    VkDestroyWindow();
}
