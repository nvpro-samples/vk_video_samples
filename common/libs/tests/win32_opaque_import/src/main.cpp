/*
 * Copyright 2024-2026 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef _WIN32

#include "Win32OpaqueImportTest.h"

#include <iostream>
#include <cstring>
#include <cstdlib>

using namespace win32_opaque_import_test;

static void printHelp(const char* prog) {
    std::cout
        << "Win32 Opaque Handle Import Test\n\n"
        << "Tests NV12 export/import via VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT\n"
        << "with every combination of usage flags (SAMPLED, STORAGE, VIDEO_ENCODE_SRC)\n"
        << "and create flags (EXTENDED_USAGE, MUTABLE_FORMAT, VIDEO_PROFILE_INDEPENDENT).\n\n"
        << "For each export configuration, two imports are attempted:\n"
        << "  - Graphics import: strips VIDEO_ENCODE_SRC and VIDEO_PROFILE_INDEPENDENT\n"
        << "  - Video import:    keeps all usage/create flags from export\n\n"
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --help, -h          Show this help\n"
        << "  --verbose, -v       Verbose output (per-operation logging)\n"
        << "  --validation        Enable Vulkan validation layers\n"
        << "  --width <N>         Image width  (default: 1920)\n"
        << "  --height <N>        Image height (default: 1080)\n\n"
        << "Example:\n"
        << "  " << prog << " --verbose --validation\n";
}

static bool parseArgs(int argc, char* argv[], TestConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printHelp(argv[0]);
            return false;
        }
        if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
            cfg.verbose = true;
        else if (!strcmp(argv[i], "--validation"))
            cfg.validation = true;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            cfg.width = static_cast<uint32_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            cfg.height = static_cast<uint32_t>(atoi(argv[++i]));
        else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printHelp(argv[0]);
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================\n"
              << " Win32 Opaque Handle Import Test Suite\n"
              << "========================================\n";

    TestConfig config;
    if (!parseArgs(argc, argv, config))
        return 1;

    Win32OpaqueImportTest test;
    VkResult result = test.init(config);
    if (result != VK_SUCCESS) {
        std::cerr << "Init failed: " << result << "\n";
        return 1;
    }

    auto results = test.runAllCombinations();

    int failures = 0;
    for (const auto& tc : results) {
        if (tc.exportCreateResult != VK_SUCCESS) continue;
        if (tc.exportAllocResult  != VK_SUCCESS) continue;
        if (tc.exportHandleResult != VK_SUCCESS) continue;
        if (tc.graphicsImport.createResult == VK_SUCCESS &&
            tc.graphicsImport.allocResult != VK_SUCCESS)
            failures++;
        if (tc.videoImport.createResult == VK_SUCCESS &&
            tc.videoImport.allocResult != VK_SUCCESS)
            failures++;
    }

    return (failures > 0) ? 1 : 0;
}

#else // !_WIN32

#include <iostream>

int main() {
    std::cout << "Win32 Opaque Handle Import Test: skipped (not Windows)\n";
    return 0;
}

#endif
