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

#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>

#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VkImageResource.h"
#include "DrmFormats.h"  // DRM format constants and mappings

namespace drm_format_mod_test {

//=============================================================================
// Test Result Types
//=============================================================================

enum class TestStatus {
    PASS,
    FAIL,
    SKIP,
    ERROR
};

struct TestResult {
    std::string     testName;
    TestStatus      status{TestStatus::SKIP};
    std::string     message;
    double          durationMs{0.0};
    
    bool passed() const { return status == TestStatus::PASS; }
    bool failed() const { return status == TestStatus::FAIL || status == TestStatus::ERROR; }
    bool skipped() const { return status == TestStatus::SKIP; }
};

//=============================================================================
// Format Support Status (for report)
//=============================================================================

enum class FormatSupportStatus {
    SUPPORTED,              // Format works with DRM modifiers
    NOT_SUPPORTED,          // Format not supported by driver
    VIDEO_DRM_FAIL,         // Video format supported but NO DRM modifier support
    LINEAR_ONLY,            // Only LINEAR modifier available
    EXPORT_FAIL,            // Export failed
    IMPORT_FAIL,            // Import failed
    UNKNOWN                 // Not tested
};

struct FormatSupportEntry {
    FormatInfo          format;
    FormatSupportStatus status{FormatSupportStatus::UNKNOWN};
    bool                hasLinear{false};
    bool                hasOptimal{false};
    bool                hasCompressed{false};
    uint32_t            modifierCount{0};
    bool                vulkanVideoSupport{false};  // Supported for decode/encode
    bool                linearExportWorks{false};
    bool                optimalExportWorks{false};
    bool                compressedExportWorks{false};
    bool                linearImportWorks{false};
    bool                optimalImportWorks{false};
    bool                compressedImportWorks{false};
    std::string         notes;
};

//=============================================================================
// Test Configuration
//=============================================================================

// Compression mode for DRM format modifier testing
enum class CompressionMode {
    Default,    // Don't touch __GL_CompressedFormatModifiers — use whatever the driver defaults to
    Enable,     // Set __GL_CompressedFormatModifiers=0x101 (GPU compressed modifiers enabled)
    Disable     // Set __GL_CompressedFormatModifiers=0x100 (swapchain only, no GPU compressed)
};

struct TestConfig {
    bool        verbose{false};
    bool        validation{false};      // Enable Vulkan validation layers
    bool        runAll{false};
    bool        listFormats{false};
    bool        generateReport{false};  // Generate comprehensive report
    bool        rgbOnly{false};
    bool        ycbcrOnly{false};
    bool        videoOnly{false};       // Only test Vulkan Video formats
    bool        exportOnly{false};      // Skip import tests
    bool        linearOnly{false};      // Only test LINEAR modifier
    CompressionMode compression{CompressionMode::Default}; // Compression mode
    std::string specificFormat;         // Test only this format
    std::string reportFile;             // Output report file path
    uint32_t    testImageWidth{256};
    uint32_t    testImageHeight{256};
};

//=============================================================================
// DrmFormatModTest - Main Test Class
//=============================================================================

class DrmFormatModTest {
public:
    DrmFormatModTest();
    ~DrmFormatModTest();
    
    // Initialize Vulkan and test infrastructure
    VkResult init(const TestConfig& config);
    
    // Run tests
    std::vector<TestResult> runAllTests();
    TestResult runFormatQueryTest(const FormatInfo& format);
    TestResult runImageCreateTest(const FormatInfo& format, bool useLinear);
    TestResult runExportImportTest(const FormatInfo& format, bool useLinear, bool useCompressed = false);
    
    // Utility functions
    bool isFormatSupported(VkFormat format) const;
    bool isDrmModifierSupported() const { return m_drmModifierSupported; }
    std::vector<DrmModifierInfo> queryDrmModifiers(VkFormat format) const;
    
    // Query Vulkan Video format support
    bool isVulkanVideoDecodeFormat(VkFormat format) const;
    bool isVulkanVideoEncodeFormat(VkFormat format) const;
    std::vector<VkFormat> getVulkanVideoFormats() const;
    
    // Print format support info
    void listSupportedFormats() const;
    
    // Generate comprehensive report
    std::vector<FormatSupportEntry> generateFormatReport();
    void printReport(const std::vector<FormatSupportEntry>& report) const;
    void saveReportToFile(const std::vector<FormatSupportEntry>& report, const std::string& filename) const;
    
private:
    // Vulkan context (member, not shared_ptr per VkCodecUtils pattern)
    VulkanDeviceContext         m_vkDevCtx;
    TestConfig                  m_config;
    
    // Physical device (for vendor-specific workarounds)
    uint32_t                    m_vendorID{0};

    // Extension support
    bool                        m_drmModifierSupported{false};
    bool                        m_dmaBufSupported{false};
    bool                        m_externalMemorySupported{false};
    bool                        m_ycbcrSupported{false};
    bool                        m_videoDecodeSupported{false};
    bool                        m_videoEncodeSupported{false};
    
    // Cached video format support
    std::vector<VkFormat>       m_videoDecodeFormats;
    std::vector<VkFormat>       m_videoEncodeFormats;
    
    // Command pool for transfer operations
    VkCommandPool               m_commandPool{VK_NULL_HANDLE};
    VkQueue                     m_queue{VK_NULL_HANDLE};
    uint32_t                    m_queueFamilyIndex{0};
    
    // Internal helpers
    VkResult createExportableImage(
        const FormatInfo& format,
        uint64_t drmModifier,
        VkSharedBaseObj<VkImageResource>& outImage);
    
    VkResult exportDmaBufFd(
        const VkSharedBaseObj<VkImageResource>& image,
        int* outFd);
    
    VkResult importDmaBufImage(
        const FormatInfo& format,
        int fd,
        VkDeviceSize size,
        uint64_t drmModifier,
        uint32_t memoryTypeBits,
        const VkSubresourceLayout* srcPlaneLayouts,
        uint32_t planeCount,
        VkSharedBaseObj<VkImageResource>& outImage);
    
    VkResult queryImageDrmModifier(
        VkImage image,
        uint64_t* outModifier);
    
    VkResult getImagePlaneLayouts(
        VkImage image,
        VkFormat format,
        std::vector<VkSubresourceLayout>& outLayouts);
    
    // Validation
    bool validateModifierProperties(
        const FormatInfo& format,
        const std::vector<DrmModifierInfo>& modifiers);
    
    bool validatePlaneLayouts(
        const FormatInfo& format,
        uint64_t modifier,
        const std::vector<VkSubresourceLayout>& layouts);
};

//=============================================================================
// Utility Functions
//=============================================================================

// Convert VkFormat to string
const char* vkFormatToString(VkFormat format);

// Convert VkFormatFeatureFlags to string
std::string formatFeaturesToString(VkFormatFeatureFlags flags);

// Convert test status to string
const char* testStatusToString(TestStatus status);

// Convert format support status to string
const char* formatSupportStatusToString(FormatSupportStatus status);

// Print test summary
void printTestSummary(const std::vector<TestResult>& results, bool verbose);

} // namespace drm_format_mod_test
