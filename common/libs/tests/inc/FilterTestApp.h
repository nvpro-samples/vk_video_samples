/*
* Copyright 2026 NVIDIA Corporation.
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

#ifndef _FILTER_TEST_APP_H_
#define _FILTER_TEST_APP_H_

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VkBufferResource.h"
#include "VkCodecUtils/VulkanFilterYuvCompute.h"
#include "VkCodecUtils/VulkanCommandBufferPool.h"

namespace vkfilter_test {

/**
 * @brief Supported test image/buffer formats
 * 
 * Value | Format  | Subsampling | Bit Depth | VkFormat
 * ------|---------|-------------|-----------|------------------------------------------
 * 0     | RGBA8   | N/A         | 8-bit     | VK_FORMAT_R8G8B8A8_UNORM
 * 1     | BGRA8   | N/A         | 8-bit     | VK_FORMAT_B8G8R8A8_UNORM
 * 2     | NV12    | 4:2:0       | 8-bit     | VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
 * 3     | P010    | 4:2:0       | 10-bit    | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
 * 4     | P012    | 4:2:0       | 12-bit    | VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16
 * 5     | I420    | 4:2:0       | 8-bit     | VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM
 * 6     | NV16    | 4:2:2       | 8-bit     | VK_FORMAT_G8_B8R8_2PLANE_422_UNORM
 * 7     | P210    | 4:2:2       | 10-bit    | VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16
 * 8     | YUV444  | 4:4:4       | 8-bit     | VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM
 * 9     | Y410    | 4:4:4       | 10-bit    | VK_FORMAT_A2B10G10R10_UNORM_PACK32 (packed)
 */
enum class TestFormat {
    // RGBA formats
    RGBA8,      // VK_FORMAT_R8G8B8A8_UNORM
    BGRA8,      // VK_FORMAT_B8G8R8A8_UNORM
    
    // 4:2:0 YCbCr formats
    NV12,       // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM (8-bit, 2-plane)
    P010,       // VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 (10-bit, 2-plane)
    P012,       // VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 (12-bit, 2-plane)
    I420,       // VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM (8-bit, 3-plane)
    
    // 4:2:2 YCbCr formats
    NV16,       // VK_FORMAT_G8_B8R8_2PLANE_422_UNORM (8-bit, 2-plane)
    P210,       // VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 (10-bit, 2-plane)
    
    // 4:4:4 YCbCr formats
    YUV444,     // VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM (8-bit, 3-plane)
    Y410,       // VK_FORMAT_A2B10G10R10_UNORM_PACK32 (10-bit, packed AVYU)
    
    COUNT       // Number of formats
};

/**
 * @brief Convert TestFormat to VkFormat
 */
VkFormat toVkFormat(TestFormat format);

/**
 * @brief Get human-readable name for TestFormat
 */
const char* testFormatName(TestFormat format);

/**
 * @brief Resource type for test I/O
 */
enum class ResourceType {
    Image,      // VkImage (storage or sampled)
    Buffer,     // VkBuffer (storage buffer)
};

/**
 * @brief Image tiling mode
 */
enum class TilingMode {
    Optimal,    // VK_IMAGE_TILING_OPTIMAL
    Linear,     // VK_IMAGE_TILING_LINEAR
};

/**
 * @brief Test I/O slot configuration
 */
struct TestIOSlot {
    TestFormat      format{TestFormat::RGBA8};
    ResourceType    resourceType{ResourceType::Image};
    TilingMode      tiling{TilingMode::Optimal};
    uint32_t        width{1920};
    uint32_t        height{1080};
    
    // For validation
    bool            generateTestPattern{true};  // Generate test pattern for inputs
    bool            validateOutput{true};       // Validate output against reference
};

/**
 * @brief Test case configuration
 */
struct TestCaseConfig {
    std::string                 name;
    VulkanFilterYuvCompute::FilterType filterType{VulkanFilterYuvCompute::RGBA2YCBCR};
    VkSamplerYcbcrModelConversion ycbcrModel{VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709};
    VkSamplerYcbcrRange         ycbcrRange{VK_SAMPLER_YCBCR_RANGE_ITU_FULL};
    
    std::vector<TestIOSlot>     inputs;
    std::vector<TestIOSlot>     outputs;
    
    float                       tolerance{0.02f};  // Validation tolerance (0.0-1.0)
    uint32_t                    filterFlags{0};    // VulkanFilterYuvCompute::FilterFlags
};

/**
 * @brief Test result
 */
struct TestResult {
    std::string     testName;
    bool            passed{false};
    std::string     errorMessage;
    double          psnrY{0.0};     // PSNR for Y plane (YCbCr) or R channel (RGBA)
    double          psnrCb{0.0};    // PSNR for Cb plane
    double          psnrCr{0.0};    // PSNR for Cr plane
    double          executionTimeMs{0.0};
};

/**
 * @brief Filter test application using VkCodecUtils infrastructure
 * 
 * This class provides a test framework for VulkanFilterYuvCompute
 * using only VkCodecUtils classes (no manual Vulkan allocations).
 */
class FilterTestApp {
public:
    FilterTestApp();
    ~FilterTestApp();
    
    /**
     * @brief Initialize Vulkan device and resources
     * @param verbose Enable verbose logging
     * @return VK_SUCCESS on success
     */
    VkResult init(bool verbose = false);
    
    /**
     * @brief Run a single test case
     * @param config Test case configuration
     * @return Test result
     */
    TestResult runTest(const TestCaseConfig& config);
    
    /**
     * @brief Run all registered test cases
     * @return Vector of test results
     */
    std::vector<TestResult> runAllTests();
    
    /**
     * @brief Register a test case
     * @param config Test case configuration
     */
    void registerTest(const TestCaseConfig& config);
    
    /**
     * @brief Get list of registered test cases
     */
    const std::vector<TestCaseConfig>& getRegisteredTests() const { return m_testCases; }
    
    /**
     * @brief Print test results summary
     * @param results Test results to summarize
     */
    static void printSummary(const std::vector<TestResult>& results);
    
    /**
     * @brief Check if a format is supported for storage images
     */
    bool isFormatSupported(TestFormat format, ResourceType resourceType, TilingMode tiling) const;

private:
    // VkCodecUtils infrastructure
    VulkanDeviceContext                      m_vkDevCtx;
    
    // Registered test cases
    std::vector<TestCaseConfig>              m_testCases;
    
    // Command buffer pool for test execution
    VkCommandPool                            m_commandPool{VK_NULL_HANDLE};
    
    /**
     * @brief Create test input resource
     */
    VkResult createTestInput(const TestIOSlot& slot,
                            VkSharedBaseObj<VkImageResource>& outImage,
                            VkSharedBaseObj<VkImageResourceView>& outImageView,
                            VkSharedBaseObj<VkBufferResource>& outBuffer);
    
    /**
     * @brief Create test output resource
     */
    VkResult createTestOutput(const TestIOSlot& slot,
                             VkSharedBaseObj<VkImageResource>& outImage,
                             VkSharedBaseObj<VkImageResourceView>& outImageView,
                             VkSharedBaseObj<VkBufferResource>& outBuffer);
    
    /**
     * @brief Create staging buffer for readback
     */
    VkResult createStagingBuffer(size_t size,
                                VkSharedBaseObj<VkBufferResource>& outBuffer);
    
    /**
     * @brief Generate test pattern in image/buffer
     */
    VkResult generateTestPattern(const TestIOSlot& slot,
                                VkSharedBaseObj<VkImageResource>& image,
                                VkSharedBaseObj<VkBufferResource>& buffer);
    
    /**
     * @brief Validate output against expected result
     */
    TestResult validateOutput(const TestCaseConfig& config,
                             const TestIOSlot& outputSlot,
                             VkSharedBaseObj<VkImageResource>& outputImage,
                             VkSharedBaseObj<VkBufferResource>& outputBuffer,
                             const std::vector<uint8_t>& referenceData);
    
    /**
     * @brief Copy image to staging buffer for CPU readback
     */
    VkResult copyImageToStagingBuffer(VkSharedBaseObj<VkImageResource>& image,
                                     VkSharedBaseObj<VkBufferResource>& stagingBuffer);
    
    /**
     * @brief Generate reference output data for validation
     */
    std::vector<uint8_t> generateReferenceOutput(const TestCaseConfig& config,
                                                 const std::vector<uint8_t>& inputData);
    
    /**
     * @brief Calculate PSNR between two buffers
     */
    double calculatePSNR(const uint8_t* data1, const uint8_t* data2, size_t size);
};

/**
 * @brief Register standard test cases
 */
void registerStandardTestCases(FilterTestApp& app);

} // namespace vkfilter_test

#endif /* _FILTER_TEST_APP_H_ */
