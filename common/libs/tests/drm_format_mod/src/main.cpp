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

#include "DrmFormatModTest.h"
#include "DrmFormats.h"

#include <iostream>
#include <cstring>
#include <cstdlib>

using namespace drm_format_mod_test;

//=============================================================================
// Help Message
//=============================================================================

static void printHelp(const char* programName) {
    std::cout << "DRM Format Modifier Test Suite\n"
              << "\n"
              << "Usage: " << programName << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --help, -h          Show this help message\n"
              << "  --verbose, -v       Enable verbose output (implies --validation)\n"
              << "  --validation        Enable Vulkan validation layers\n"
              << "  --all               Run all tests (default: smoke tests only)\n"
              << "  --list-formats      List formats with DRM modifier support and exit\n"
              << "  --format <name>     Test specific format (e.g., NV12, P010, RGBA8)\n"
              << "  --rgb-only          Test only RGB formats\n"
              << "  --ycbcr-only        Test only YCbCr formats\n"
              << "  --video-only        Test only Vulkan Video formats (8/10/12 bit YCbCr)\n"
              << "  --linear-only       Only test LINEAR modifier\n"
              << "  --export-only       Skip import tests (export only)\n"
              << "  --compression <m>   Compression mode: default, enable, disable\n"
              << "                        default  - use driver defaults (no env var change)\n"
              << "                        enable   - set __GL_CompressedFormatModifiers=0x101\n"
              << "                                   (advertise GPU compressed DRM modifiers)\n"
              << "                        disable  - set __GL_CompressedFormatModifiers=0x100\n"
              << "                                   (swapchain only, no GPU compressed modifiers)\n"
              << "  --report            Generate comprehensive format support report\n"
              << "  --report-file <f>   Save report to file (default: drm_format_report.md)\n"
              << "  --width <N>         Test image width (default: 256)\n"
              << "  --height <N>        Test image height (default: 256)\n"
              << "\n"
              << "Report features:\n"
              << "  - Shows all supported/unsupported formats\n"
              << "  - Marks Vulkan Video formats (decode/encode 8/10/12 bit)\n"
              << "  - Flags VIDEO_DRM_FAIL when video format lacks DRM support\n"
              << "\n"
              << "Compression:\n"
              << "  NVIDIA GPUs support L2/XBAR framebuffer compression via DRM modifiers.\n"
              << "  Compressed modifiers have compressionType != 0 (bits 25:23) and use\n"
              << "  NV_MMU_PTE_KIND_GENERIC_MEMORY_COMPRESSIBLE as pageKind.\n"
              << "  By default, GPU compressed modifiers are not advertised (only swapchain).\n"
              << "  Use --compression enable to test compressed export/import round-trips.\n"
              << "\n"
              << "Examples:\n"
              << "  " << programName << "                              # Run smoke tests\n"
              << "  " << programName << " --all --verbose              # Run all tests with verbose output\n"
              << "  " << programName << " --format NV12                # Test NV12 format only\n"
              << "  " << programName << " --list-formats               # List supported formats\n"
              << "  " << programName << " --ycbcr-only                 # Test YCbCr formats only\n"
              << "  " << programName << " --compression enable --all   # Test with compressed modifiers\n"
              << "  " << programName << " --compression disable --all  # Test without compressed modifiers\n"
              << "  " << programName << " --report --verbose           # Generate detailed report\n"
              << "  " << programName << " --video-only --report        # Report on video formats\n"
              << std::endl;
}

//=============================================================================
// Parse Command Line
//=============================================================================

static bool parseArgs(int argc, char* argv[], TestConfig& config) {
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printHelp(argv[0]);
            return false;
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            config.verbose = true;
        }
        else if (strcmp(arg, "--validation") == 0) {
            config.validation = true;
        }
        else if (strcmp(arg, "--all") == 0) {
            config.runAll = true;
        }
        else if (strcmp(arg, "--list-formats") == 0) {
            config.listFormats = true;
        }
        else if (strcmp(arg, "--rgb-only") == 0) {
            config.rgbOnly = true;
        }
        else if (strcmp(arg, "--ycbcr-only") == 0) {
            config.ycbcrOnly = true;
        }
        else if (strcmp(arg, "--linear-only") == 0) {
            config.linearOnly = true;
        }
        else if (strcmp(arg, "--export-only") == 0) {
            config.exportOnly = true;
        }
        else if (strcmp(arg, "--compression") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            if (strcmp(mode, "default") == 0) {
                config.compression = CompressionMode::Default;
            } else if (strcmp(mode, "enable") == 0) {
                config.compression = CompressionMode::Enable;
            } else if (strcmp(mode, "disable") == 0) {
                config.compression = CompressionMode::Disable;
            } else {
                std::cerr << "Error: Invalid compression mode '" << mode 
                          << "'. Use: default, enable, disable" << std::endl;
                return false;
            }
        }
        else if (strcmp(arg, "--video-only") == 0) {
            config.videoOnly = true;
        }
        else if (strcmp(arg, "--report") == 0) {
            config.generateReport = true;
        }
        else if (strcmp(arg, "--report-file") == 0 && i + 1 < argc) {
            config.reportFile = argv[++i];
            config.generateReport = true;  // Implies report generation
        }
        else if (strcmp(arg, "--format") == 0 && i + 1 < argc) {
            config.specificFormat = argv[++i];
        }
        else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            config.testImageWidth = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            config.testImageHeight = static_cast<uint32_t>(atoi(argv[++i]));
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printHelp(argv[0]);
            return false;
        }
    }
    
    // Validate conflicting options
    if (config.rgbOnly && config.ycbcrOnly) {
        std::cerr << "Error: --rgb-only and --ycbcr-only are mutually exclusive" << std::endl;
        return false;
    }
    
    if ((config.rgbOnly || config.ycbcrOnly) && config.videoOnly) {
        std::cerr << "Error: --video-only cannot be combined with --rgb-only or --ycbcr-only" << std::endl;
        return false;
    }
    
    // Validate dimensions
    if (config.testImageWidth == 0 || config.testImageHeight == 0) {
        std::cerr << "Error: Invalid image dimensions" << std::endl;
        return false;
    }
    
    return true;
}

//=============================================================================
// Main
//=============================================================================

int main(int argc, char* argv[]) {
    std::cout << "======================================" << std::endl;
    std::cout << " DRM Format Modifier Test Suite" << std::endl;
    std::cout << "======================================" << std::endl;
    
    TestConfig config;
    
    if (!parseArgs(argc, argv, config)) {
        return 1;
    }
    
    // Apply compression mode BEFORE Vulkan init (affects driver modifier enumeration)
    // __GL_CompressedFormatModifiers is NVIDIA-specific. On Intel/AMD this env var
    // is ignored — their drivers use different mechanisms for compression (CCS/DCC).
    // The --compression flag is still useful: it controls modifier selection logic
    // in the test (isCompressed() checks vendor prefix before interpreting bits).
    switch (config.compression) {
        case CompressionMode::Enable:
            // NVIDIA: Bit 0 = GPU_SUPPORTED, Bit 8 = SWAPCHAIN_SUPPORTED
            setenv("__GL_CompressedFormatModifiers", "0x101", 1);
            std::cout << "[INFO] Compression: ENABLED (NVIDIA: __GL_CompressedFormatModifiers=0x101)" << std::endl;
            break;
        case CompressionMode::Disable:
            // NVIDIA: Bit 8 only = SWAPCHAIN_SUPPORTED (no GPU compressed modifiers)
            setenv("__GL_CompressedFormatModifiers", "0x100", 1);
            std::cout << "[INFO] Compression: DISABLED (NVIDIA: __GL_CompressedFormatModifiers=0x100)" << std::endl;
            break;
        case CompressionMode::Default:
        default: {
            const char* existing = getenv("__GL_CompressedFormatModifiers");
            std::cout << "[INFO] Compression: DEFAULT (env=" 
                      << (existing ? existing : "not set") << ")" << std::endl;
            break;
        }
    }
    
    // Create and initialize test app
    DrmFormatModTest testApp;
    
    VkResult result = testApp.init(config);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to initialize test application: " << result << std::endl;
        return 1;
    }
    
    // List formats mode
    if (config.listFormats) {
        testApp.listSupportedFormats();
        return 0;
    }
    
    // Report generation mode
    if (config.generateReport) {
        auto report = testApp.generateFormatReport();
        testApp.printReport(report);
        
        // Save to file if specified
        std::string reportFile = config.reportFile.empty() ? 
                                 "drm_format_report.md" : config.reportFile;
        testApp.saveReportToFile(report, reportFile);
        
        // Check for VIDEO_DRM_FAIL entries
        int videoDrmFailures = 0;
        for (const auto& e : report) {
            if (e.status == FormatSupportStatus::VIDEO_DRM_FAIL) {
                videoDrmFailures++;
            }
        }
        
        if (videoDrmFailures > 0) {
            std::cerr << "\n*** " << videoDrmFailures 
                      << " VIDEO FORMAT DRM FAILURE(S) DETECTED ***" << std::endl;
            return 1;
        }
        
        return 0;
    }
    
    // Run tests
    auto results = testApp.runAllTests();
    
    // Count failures
    int failures = 0;
    for (const auto& r : results) {
        if (r.failed()) {
            failures++;
        }
    }
    
    return (failures > 0) ? 1 : 0;
}
