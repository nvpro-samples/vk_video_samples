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

#if defined(__linux__)

#include "LinuxDmaBufImportTest.h"

#include <strings.h>   // strcasecmp

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace linux_dmabuf_import_test;

// Exit codes are three-valued on purpose. "The test could not run" must not be
// spellable as success by anything reading $? - see the banner below.
static const int kExitClaimVerified = 0;
static const int kExitClaimFailed   = 1;
static const int kExitCouldNotRun   = 2;
static const int kExitBadUsage      = 64;

static void printHelp(const char* prog) {
    std::cout
        << "Linux dma-buf SECOND-DEVICE import test\n\n"
        << "Exports an image from one VkDevice and imports it into a DIFFERENT\n"
        << "VkDevice on the SAME VkPhysicalDevice, then proves the memory is\n"
        << "usable by writing a pattern through device A and reading it back\n"
        << "through device B.\n\n"
        << "This is the Linux counterpart of win32_opaque_import.\n\n"
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --help, -h           Show this help\n"
        << "  --verbose, -v        Per-operation logging\n"
        << "  --validation         Enable Vulkan validation layers\n"
        << "  --width <N>          Image width  (default 1920)\n"
        << "  --height <N>         Image height (default 1080)\n"
        << "  --format <name>      nv12 | nv16 | p010 | p012 | rgba8 | bgra8\n"
        << "                       (default nv12)\n"
        << "  --no-video-usage     Drop VIDEO_ENCODE_SRC and the video create flags\n"
        << "                       on BOTH the export and the import image\n"
        << "  --import-no-video-usage\n"
        << "                       Drop them on the IMPORT image only. This is the\n"
        << "                       Chromium shape: a foreign (GBM) exporter, and an\n"
        << "                       importer whose usage the VEA picked itself.\n"
        << "  --import-video-usage Force them ON for the import image only\n"
        << "  --import-usage <hex> Raw VkImageUsageFlags for the import image\n"
        << "  --import-flags <hex> Raw VkImageCreateFlags for the import image\n"
        << "  --linear-only        Only test DRM_FORMAT_MOD_LINEAR\n"
        << "  --modifier <hex>     Only test this DRM modifier (e.g. 0x300000000c000001)\n"
        << "  --no-content-check   Stop after bind; do NOT prove the memory is\n"
        << "                       usable. Reports COULD-NOT-RUN, never PASS.\n"
        << "  --no-control         Skip the same-device control arm\n\n"
        << "Exit codes:\n"
        << "  0  claim verified   (second-device import + content round-trip OK)\n"
        << "  1  claim FAILED\n"
        << "  2  could not run    (no GPU / no extension / nothing proved)\n"
        << "  64 bad usage\n";
}

static bool parseFormat(const char* s, VkFormat& out) {
    if (!strcasecmp(s, "nv12"))  { out = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; return true; }
    if (!strcasecmp(s, "nv16"))  { out = VK_FORMAT_G8_B8R8_2PLANE_422_UNORM; return true; }
    if (!strcasecmp(s, "p010"))  { out = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16; return true; }
    if (!strcasecmp(s, "p012"))  { out = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16; return true; }
    if (!strcasecmp(s, "rgba8")) { out = VK_FORMAT_R8G8B8A8_UNORM; return true; }
    if (!strcasecmp(s, "bgra8")) { out = VK_FORMAT_B8G8R8A8_UNORM; return true; }
    return false;
}

static bool parseArgs(int argc, char* argv[], TestConfig& cfg, bool& helpShown) {
    helpShown = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printHelp(argv[0]);
            helpShown = true;
            return false;
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            cfg.verbose = true;
        } else if (!strcmp(a, "--validation")) {
            cfg.validation = true;
        } else if (!strcmp(a, "--no-video-usage")) {
            cfg.videoUsage = false;
        } else if (!strcmp(a, "--import-no-video-usage")) {
            cfg.importVideoUsage = 0;
        } else if (!strcmp(a, "--import-video-usage")) {
            cfg.importVideoUsage = 1;
        } else if (!strcmp(a, "--import-usage") && (i + 1 < argc)) {
            cfg.importUsageRaw = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (!strcmp(a, "--import-flags") && (i + 1 < argc)) {
            cfg.importFlagsRaw = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (!strcmp(a, "--linear-only")) {
            cfg.linearOnly = true;
        } else if (!strcmp(a, "--no-content-check")) {
            cfg.contentCheck = false;
        } else if (!strcmp(a, "--no-control")) {
            cfg.sameDeviceControl = false;
        } else if (!strcmp(a, "--width") && (i + 1 < argc)) {
            cfg.width = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (!strcmp(a, "--height") && (i + 1 < argc)) {
            cfg.height = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (!strcmp(a, "--modifier") && (i + 1 < argc)) {
            cfg.onlyModifier = strtoull(argv[++i], nullptr, 0);
        } else if (!strcmp(a, "--format") && (i + 1 < argc)) {
            if (!parseFormat(argv[++i], cfg.format)) {
                std::cerr << "Unknown format: " << argv[i] << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            printHelp(argv[0]);
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "=================================================\n"
              << " Linux dma-buf SECOND-DEVICE import test\n"
              << "=================================================\n";

    TestConfig config;
    bool helpShown = false;
    if (!parseArgs(argc, argv, config, helpShown)) {
        return helpShown ? kExitClaimVerified : kExitBadUsage;
    }

    LinuxDmaBufImportTest test;
    VkResult initResult = test.init(config);
    if (initResult != VK_SUCCESS) {
        std::cerr << "\n" << std::string(72, '!') << "\n"
                  << "COULD NOT RUN - THIS IS NOT A PASS\n"
                  << std::string(72, '!') << "\n"
                  << "  stopped at : " << stepName(test.initFailedAt()) << "\n"
                  << "  VkResult   : " << vkResultName(initResult) << "\n"
                  << "  detail     : " << test.initFailureDetail() << "\n\n"
                  << "The second-device dma-buf import claim is UNTESTED\n"
                  << "on this host. Do not read this run as evidence for or\n"
                  << "against it.\n";
        return kExitCouldNotRun;
    }

    std::vector<ArmResult> results = test.run();
    test.printResults(results);

    std::string reason;
    const Verdict v = test.verdict(results, reason);

    switch (v) {
        case Verdict::ClaimVerified:
            std::cout << std::string(72, '=') << "\n"
                      << "VERDICT: CLAIM VERIFIED\n"
                      << std::string(72, '=') << "\n"
                      << "  " << reason << "\n";
            return kExitClaimVerified;

        case Verdict::ClaimFailed:
            std::cout << std::string(72, '!') << "\n"
                      << "VERDICT: CLAIM FAILED\n"
                      << std::string(72, '!') << "\n"
                      << "  " << reason << "\n";
            return kExitClaimFailed;

        case Verdict::CouldNotRun:
        default:
            std::cout << std::string(72, '!') << "\n"
                      << "VERDICT: COULD NOT RUN - THIS IS NOT A PASS\n"
                      << std::string(72, '!') << "\n"
                      << "  " << reason << "\n";
            return kExitCouldNotRun;
    }
}

#else // !__linux__

#include <iostream>

// dma-buf is a Linux kernel object; there is no cross-platform equivalent to
// test here. The Windows sibling is common/libs/tests/win32_opaque_import.
int main() {
    std::cout << "Linux dma-buf second-device import test: NOT APPLICABLE "
                 "(dma-buf is Linux-only). See win32_opaque_import for the "
                 "Windows OPAQUE_WIN32 sibling.\n";
    return 0;
}

#endif // __linux__
