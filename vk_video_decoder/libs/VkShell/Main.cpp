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

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

#include "FrameProcessorFactory.h"

struct Args {
    uint32_t device_id;
    uint32_t direct_mode:1;
    Args()
        : device_id(),
          direct_mode(false) {

    }
};

bool scan_args(int argc, char* argv[], Args& out) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-deviceID")) {
            if (i + 1 == argc) {
                std::printf("Missing argument for -deviceID\n");
                return false;
            }
            std::sscanf(argv[i + 1], "%x", &out.device_id);
            ++i;
        } else if (!std::strcmp(argv[i], "--direct")) {
            out.direct_mode = true;
        }
    }
    return true;
}

#if defined(VK_USE_PLATFORM_XCB_KHR)

#include "ShellXcb.h"
#include "ShellDirect.h"

int main(int argc, char **argv) {
    Args a;
    if (!scan_args(argc, argv, a)) return -1;

    FrameProcessor *frameProcessor = create_frameProcessor(argc, argv);
    {
        if (a.direct_mode) {
            ShellDirect shell(*frameProcessor, a.device_id);
            shell.run();
        } else {
            ShellXcb shell(*frameProcessor, a.device_id);
            shell.run();
        }
    }
    delete frameProcessor;

    return 0;
}

#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)

#include "ShellWayland.h"

int main(int argc, char** argv) {
    Args a;
    if (!scan_args(argc, argv, a)) return -1;

    FrameProcessor* frameProcessor = create_frameProcessor(argc, argv);
    {
        ShellWayland shell(*frameProcessor, a.device_id);
        shell.run();
    }
    delete frameProcessor;

    return 0;
}

#elif defined(VK_USE_PLATFORM_ANDROID_KHR)

#include <android/log.h>
#include "ShellAndroid.h"

void android_main(android_app *app) {
    FrameProcessor *frameProcessor = create_frameProcessor(ShellAndroid::get_args(*app));

    try {
        ShellAndroid shell(*app, *frameProcessor);
        shell.run();
    } catch (const std::runtime_error &e) {
        __android_log_print(ANDROID_LOG_ERROR, frameProcessor->settings().name.c_str(), "%s", e.what());
    }

    delete frameProcessor;
}

#elif defined(VK_USE_PLATFORM_WIN32_KHR)

#include "ShellWin32.h"

int main(int argc, char** argv) {
    Args a;
    if (!scan_args(argc, argv, a)) return -1;

    FrameProcessor* frameProcessor = create_frameProcessor(argc, argv);
    {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        ShellWin32 shell(*frameProcessor, a.device_id);
#else
        ShellXcb shell(*frameProcessor);
#endif
        shell.run();
    }
    delete frameProcessor;

    return 0;
}

#endif  // VK_USE_PLATFORM_XCB_KHR
