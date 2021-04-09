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

#ifndef FRAMEPROCESSOR_H
#define FRAMEPROCESSOR_H

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

class Shell;

class FrameProcessor {
   public:
    FrameProcessor(const FrameProcessor &frameProcessor) = delete;
    FrameProcessor &operator=(const FrameProcessor &frameProcessor) = delete;
    virtual ~FrameProcessor() {}

    struct Settings {
        std::string name;
        int initial_width;
        int initial_height;
        int video_width;
        int video_height;
        int queue_count;
        int back_buffer_count;
        int ticks_per_second;
        bool vsync;
        bool animate;

        bool validate;
        bool validate_verbose;

        bool no_tick;
        bool no_render;
        bool no_present;

        // Whether or not to use VkFlushMappedMemoryRanges
        bool flush_buffers;

        int max_frame_count;

        std::string videoFileName;
        int gpuIndex;
    };
    const Settings &settings() const { return settings_; }

    virtual int attach_shell(Shell &shell) { shell_ = &shell; return 0; }
    virtual void detach_shell() { shell_ = nullptr; }

    virtual int attach_swapchain() { return 0; }
    virtual void detach_swapchain() {}

    virtual bool requires_vulkan_video() { return false; }

    enum Key {
        // virtual keys
        KEY_SHUTDOWN,
        // physical keys
        KEY_UNKNOWN,
        KEY_ESC,
        KEY_UP,
        KEY_DOWN,
        KEY_LEFT,
        KEY_RIGHT,
        KEY_PAGE_UP,
        KEY_PAGE_DOWN,
        KEY_SPACE,
    };
    virtual void on_key(Key key) {}
    virtual void on_tick() {}

    virtual void on_frame(bool trainFrame = false) {}

    void print_stats();
    void quit();

   protected:
    int frame_count;
    std::chrono::time_point<std::chrono::steady_clock> start_time;

    FrameProcessor(const std::string &name, const std::vector<std::string> &args) : settings_(), shell_(nullptr) {
        settings_.name = name;
        settings_.initial_width = 1920;
        settings_.initial_height = 1080;
        settings_.video_width = 0;
        settings_.video_height = 0;
        settings_.queue_count = 1;
        settings_.back_buffer_count = 3;
        settings_.ticks_per_second = 30;
        settings_.vsync = true;
        settings_.animate = true;

        settings_.validate = false;
        settings_.validate_verbose = false;

        settings_.no_tick = false;
        settings_.no_render = false;
        settings_.no_present = false;

        settings_.flush_buffers = false;

        settings_.max_frame_count = -1;

        parse_args(args);

        frame_count = 0;
        // Record start time for printing stats later
        start_time = std::chrono::steady_clock::now();

        // std::cout << "The clock resolution is: " << (unsigned long long) std::chrono::high_resolution_clock::period::num / std::chrono::high_resolution_clock::period::den;
    }

    Settings settings_;
    Shell *shell_;

   private:
    void parse_args(const std::vector<std::string> &args) {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == "--b") {
                settings_.vsync = false;
            } else if (*it == "--w") {
                ++it;
                settings_.initial_width = std::stoi(*it);
            } else if (*it == "--h") {
                ++it;
                settings_.initial_height = std::stoi(*it);
            } else if (*it == "--v") {
                settings_.validate = true;
            } else if (*it == "--validate") {
                settings_.validate = true;
            } else if (*it == "--vv") {
                settings_.validate = true;
                settings_.validate_verbose = true;
            } else if (*it == "--nt") {
                settings_.no_tick = true;
            } else if (*it == "--nr") {
                settings_.no_render = true;
            } else if (*it == "--np") {
                settings_.no_present = true;
            } else if (*it == "--flush") {
                settings_.flush_buffers = true;
            } else if (*it == "-i") {
                it++;
                settings_.videoFileName = *it;
            } else if (*it == "--gpu") {
                it++;
                settings_.gpuIndex = std::atoi(it->c_str());
            } else if (*it == "--c") {
                ++it;
                settings_.max_frame_count = std::stoi(*it);
            }
        }
    }
};

#endif  // FRAMEPROCESSOR_H
