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

#include "TestCases.h"

namespace vkfilter_test {
namespace TestCases {

// =============================================================================
// Helper to create RGBA to YCbCr test config
// =============================================================================
static TestCaseConfig createRGBA2YCbCr(const char* name, TestFormat outputFormat,
                                       VkSamplerYcbcrModelConversion model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                                       VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
                                       uint32_t width = 1920, uint32_t height = 1080) {
    TestCaseConfig config;
    config.name = name;
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = model;
    config.ycbcrRange = range;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = true,
        .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = outputFormat,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// Helper to create YCbCr to RGBA test config
// =============================================================================
static TestCaseConfig createYCbCr2RGBA(const char* name, TestFormat inputFormat,
                                       VkSamplerYcbcrModelConversion model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                                       VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
                                       uint32_t width = 1920, uint32_t height = 1080) {
    TestCaseConfig config;
    config.name = name;
    config.filterType = VulkanFilterYuvCompute::YCBCR2RGBA;
    config.ycbcrModel = model;
    config.ycbcrRange = range;
    
    config.inputs.push_back({
        .format = inputFormat,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = true,
        .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// Helper to create YCbCr copy test config
// =============================================================================
static TestCaseConfig createYCbCrCopy(const char* name, TestFormat format,
                                      uint32_t width = 1920, uint32_t height = 1080) {
    TestCaseConfig config;
    config.name = name;
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = true,
        .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// Helper to create YCbCr clear test config
// =============================================================================
static TestCaseConfig createYCbCrClear(const char* name, TestFormat format,
                                       uint32_t width = 1920, uint32_t height = 1080) {
    TestCaseConfig config;
    config.name = name;
    config.filterType = VulkanFilterYuvCompute::YCBCRCLEAR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// RGBA to YCbCr Conversion Tests (All 8 YCbCr formats)
// =============================================================================

// 4:2:0 formats
TestCaseConfig TC001_RGBA_to_NV12() {
    return createRGBA2YCbCr("TC001_RGBA_to_NV12", TestFormat::NV12);
}

TestCaseConfig TC002_RGBA_to_P010() {
    return createRGBA2YCbCr("TC002_RGBA_to_P010", TestFormat::P010);
}

TestCaseConfig TC003_RGBA_to_P012() {
    return createRGBA2YCbCr("TC003_RGBA_to_P012", TestFormat::P012);
}

TestCaseConfig TC004_RGBA_to_I420() {
    return createRGBA2YCbCr("TC004_RGBA_to_I420", TestFormat::I420);
}

// 4:2:2 formats
TestCaseConfig TC005_RGBA_to_NV16() {
    return createRGBA2YCbCr("TC005_RGBA_to_NV16", TestFormat::NV16);
}

TestCaseConfig TC006_RGBA_to_P210() {
    return createRGBA2YCbCr("TC006_RGBA_to_P210", TestFormat::P210);
}

// 4:4:4 formats
TestCaseConfig TC007_RGBA_to_YUV444() {
    return createRGBA2YCbCr("TC007_RGBA_to_YUV444", TestFormat::YUV444);
}

TestCaseConfig TC008_RGBA_to_Y410() {
    return createRGBA2YCbCr("TC008_RGBA_to_Y410", TestFormat::Y410);
}

// =============================================================================
// YCbCr to RGBA Conversion Tests (All 8 YCbCr formats)
// =============================================================================

// 4:2:0 formats
TestCaseConfig TC010_NV12_to_RGBA() {
    return createYCbCr2RGBA("TC010_NV12_to_RGBA", TestFormat::NV12);
}

TestCaseConfig TC011_P010_to_RGBA() {
    return createYCbCr2RGBA("TC011_P010_to_RGBA", TestFormat::P010);
}

TestCaseConfig TC012_P012_to_RGBA() {
    return createYCbCr2RGBA("TC012_P012_to_RGBA", TestFormat::P012);
}

TestCaseConfig TC013_I420_to_RGBA() {
    return createYCbCr2RGBA("TC013_I420_to_RGBA", TestFormat::I420);
}

// 4:2:2 formats
TestCaseConfig TC014_NV16_to_RGBA() {
    return createYCbCr2RGBA("TC014_NV16_to_RGBA", TestFormat::NV16);
}

TestCaseConfig TC015_P210_to_RGBA() {
    return createYCbCr2RGBA("TC015_P210_to_RGBA", TestFormat::P210);
}

// 4:4:4 formats
TestCaseConfig TC016_YUV444_to_RGBA() {
    return createYCbCr2RGBA("TC016_YUV444_to_RGBA", TestFormat::YUV444);
}

TestCaseConfig TC017_Y410_to_RGBA() {
    return createYCbCr2RGBA("TC017_Y410_to_RGBA", TestFormat::Y410);
}

// =============================================================================
// Color Primaries Tests (BT.601, BT.709, BT.2020)
// =============================================================================

TestCaseConfig TC020_RGBA_to_NV12_BT601() {
    return createRGBA2YCbCr("TC020_RGBA_to_NV12_BT601", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601);
}

TestCaseConfig TC021_RGBA_to_NV12_BT709() {
    return createRGBA2YCbCr("TC021_RGBA_to_NV12_BT709", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709);
}

TestCaseConfig TC022_RGBA_to_NV12_BT2020() {
    return createRGBA2YCbCr("TC022_RGBA_to_NV12_BT2020", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020);
}

TestCaseConfig TC023_RGBA_to_P010_BT601() {
    return createRGBA2YCbCr("TC023_RGBA_to_P010_BT601", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601);
}

TestCaseConfig TC024_RGBA_to_P010_BT709() {
    return createRGBA2YCbCr("TC024_RGBA_to_P010_BT709", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709);
}

TestCaseConfig TC025_RGBA_to_P010_BT2020() {
    return createRGBA2YCbCr("TC025_RGBA_to_P010_BT2020", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020);
}

// =============================================================================
// Range Tests (Full vs Limited)
// =============================================================================

TestCaseConfig TC030_RGBA_to_NV12_FullRange() {
    return createRGBA2YCbCr("TC030_RGBA_to_NV12_FullRange", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL);
}

TestCaseConfig TC031_RGBA_to_NV12_LimitedRange() {
    return createRGBA2YCbCr("TC031_RGBA_to_NV12_LimitedRange", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
}

TestCaseConfig TC032_RGBA_to_P010_FullRange() {
    return createRGBA2YCbCr("TC032_RGBA_to_P010_FullRange", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL);
}

TestCaseConfig TC033_RGBA_to_P010_LimitedRange() {
    return createRGBA2YCbCr("TC033_RGBA_to_P010_LimitedRange", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
}

// =============================================================================
// YCbCr Copy Tests (Same format in → out)
// =============================================================================

TestCaseConfig TC040_YCbCrCopy_NV12() {
    return createYCbCrCopy("TC040_YCbCrCopy_NV12", TestFormat::NV12);
}

TestCaseConfig TC041_YCbCrCopy_P010() {
    return createYCbCrCopy("TC041_YCbCrCopy_P010", TestFormat::P010);
}

TestCaseConfig TC042_YCbCrCopy_I420() {
    return createYCbCrCopy("TC042_YCbCrCopy_I420", TestFormat::I420);
}

TestCaseConfig TC043_YCbCrCopy_NV16() {
    return createYCbCrCopy("TC043_YCbCrCopy_NV16", TestFormat::NV16);
}

TestCaseConfig TC044_YCbCrCopy_YUV444() {
    return createYCbCrCopy("TC044_YCbCrCopy_YUV444", TestFormat::YUV444);
}

// =============================================================================
// YCbCr Clear Tests
// =============================================================================

TestCaseConfig TC050_YCbCrClear_NV12() {
    return createYCbCrClear("TC050_YCbCrClear_NV12", TestFormat::NV12);
}

TestCaseConfig TC051_YCbCrClear_P010() {
    return createYCbCrClear("TC051_YCbCrClear_P010", TestFormat::P010);
}

// =============================================================================
// YCbCr Format Conversion Tests (Different YCbCr in → out)
// =============================================================================

TestCaseConfig TC060_NV12_to_I420() {
    TestCaseConfig config;
    config.name = "TC060_NV12_to_I420";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;  // Format conversion uses copy path
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::I420,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC061_I420_to_NV12() {
    TestCaseConfig config;
    config.name = "TC061_I420_to_NV12";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::I420,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC062_NV12_to_NV16() {
    TestCaseConfig config;
    config.name = "TC062_NV12_to_NV16";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV16,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC063_NV12_to_YUV444() {
    TestCaseConfig config;
    config.name = "TC063_NV12_to_YUV444";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::YUV444,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC064_P010_to_NV12() {
    TestCaseConfig config;
    config.name = "TC064_P010_to_NV12";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::P010,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC065_NV12_to_P010() {
    TestCaseConfig config;
    config.name = "TC065_NV12_to_P010";
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::P010,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

// =============================================================================
// Buffer I/O Tests
// =============================================================================

TestCaseConfig TC070_RGBABuffer_to_NV12Image() {
    TestCaseConfig config;
    config.name = "TC070_RGBABuffer_to_NV12Image";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC071_RGBAImage_to_NV12Buffer() {
    TestCaseConfig config;
    config.name = "TC071_RGBAImage_to_NV12Buffer";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC072_RGBABuffer_to_NV12Buffer() {
    TestCaseConfig config;
    config.name = "TC072_RGBABuffer_to_NV12Buffer";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC073_NV12Buffer_to_RGBAImage() {
    TestCaseConfig config;
    config.name = "TC073_NV12Buffer_to_RGBAImage";
    config.filterType = VulkanFilterYuvCompute::YCBCR2RGBA;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC074_NV12Image_to_RGBABuffer() {
    TestCaseConfig config;
    config.name = "TC074_NV12Image_to_RGBABuffer";
    config.filterType = VulkanFilterYuvCompute::YCBCR2RGBA;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC075_RGBABuffer_to_P010Buffer() {
    TestCaseConfig config;
    config.name = "TC075_RGBABuffer_to_P010Buffer";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::P010,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

TestCaseConfig TC076_P010Buffer_to_RGBABuffer() {
    TestCaseConfig config;
    config.name = "TC076_P010Buffer_to_RGBABuffer";
    config.filterType = VulkanFilterYuvCompute::YCBCR2RGBA;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::P010,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    config.outputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    return config;
}

// =============================================================================
// Linear Tiling Tests
// =============================================================================

TestCaseConfig TC080_RGBA_to_NV12_Linear() {
    TestCaseConfig config = createRGBA2YCbCr("TC080_RGBA_to_NV12_Linear", TestFormat::NV12);
    config.outputs[0].tiling = TilingMode::Linear;
    return config;
}

TestCaseConfig TC081_RGBA_to_P010_Linear() {
    TestCaseConfig config = createRGBA2YCbCr("TC081_RGBA_to_P010_Linear", TestFormat::P010);
    config.outputs[0].tiling = TilingMode::Linear;
    return config;
}

TestCaseConfig TC082_Linear_NV12_to_Optimal_NV12() {
    TestCaseConfig config = createYCbCrCopy("TC082_Linear_NV12_to_Optimal_NV12", TestFormat::NV12);
    config.inputs[0].tiling = TilingMode::Linear;
    config.outputs[0].tiling = TilingMode::Optimal;
    return config;
}

TestCaseConfig TC083_Optimal_NV12_to_Linear_NV12() {
    TestCaseConfig config = createYCbCrCopy("TC083_Optimal_NV12_to_Linear_NV12", TestFormat::NV12);
    config.inputs[0].tiling = TilingMode::Optimal;
    config.outputs[0].tiling = TilingMode::Linear;
    return config;
}

// =============================================================================
// Multi-Output Tests (Future - for flexible I/O)
// =============================================================================

TestCaseConfig TC090_Dual_Output_Optimal_Linear() {
    TestCaseConfig config;
    config.name = "TC090_Dual_Output_Optimal_Linear";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    // Output 0: Optimal for encoder
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    // Output 1: Linear for dumper
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

TestCaseConfig TC091_Triple_Output_with_Subsampled() {
    TestCaseConfig config = TC090_Dual_Output_Optimal_Linear();
    config.name = "TC091_Triple_Output_with_Subsampled";
    config.filterFlags = VulkanFilterYuvCompute::FLAG_ENABLE_Y_SUBSAMPLING;
    
    // Output 2: 2x2 subsampled Y for AQ
    config.outputs.push_back({
        .format = TestFormat::NV12,  // Placeholder - actually R8
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 960, .height = 540,  // Half resolution
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// Transfer Operation Tests (Pre/Post Transfer scenarios)
// =============================================================================

// Helper for transfer tests
static TestCaseConfig createTransferTest(const char* name, 
                                         TestFormat format,
                                         bool preTransfer,
                                         bool postTransfer,
                                         TilingMode srcTiling = TilingMode::Optimal,
                                         TilingMode dstTiling = TilingMode::Optimal,
                                         uint32_t width = 1920, uint32_t height = 1080) {
    TestCaseConfig config;
    config.name = name;
    config.filterType = VulkanFilterYuvCompute::YCBCRCOPY;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    // Enable pre/post transfer flags
    uint32_t flags = 0;
    if (preTransfer) {
        flags |= VulkanFilterYuvCompute::FLAG_PRE_TRANSFER;
    }
    if (postTransfer) {
        flags |= VulkanFilterYuvCompute::FLAG_POST_TRANSFER;
    }
    config.filterFlags = flags;
    
    config.inputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = srcTiling,
        .width = width,
        .height = height,
        .generateTestPattern = true,
        .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = format,
        .resourceType = ResourceType::Image,
        .tiling = dstTiling,
        .width = width,
        .height = height,
        .generateTestPattern = false,
        .validateOutput = true
    });
    
    return config;
}

// Pre-transfer: Linear source copied to optimal before compute
TestCaseConfig TC110_PreTransfer_LinearToOptimal_NV12() {
    return createTransferTest("TC110_PreTransfer_LinearToOptimal_NV12", 
                             TestFormat::NV12, true, false,
                             TilingMode::Linear, TilingMode::Optimal);
}

TestCaseConfig TC111_PreTransfer_LinearToOptimal_P010() {
    return createTransferTest("TC111_PreTransfer_LinearToOptimal_P010", 
                             TestFormat::P010, true, false,
                             TilingMode::Linear, TilingMode::Optimal);
}

// Post-transfer: Optimal output copied to linear after compute
TestCaseConfig TC112_PostTransfer_OptimalToLinear_NV12() {
    return createTransferTest("TC112_PostTransfer_OptimalToLinear_NV12", 
                             TestFormat::NV12, false, true,
                             TilingMode::Optimal, TilingMode::Linear);
}

TestCaseConfig TC113_PostTransfer_OptimalToLinear_P010() {
    return createTransferTest("TC113_PostTransfer_OptimalToLinear_P010", 
                             TestFormat::P010, false, true,
                             TilingMode::Optimal, TilingMode::Linear);
}

// Both pre and post transfer: Linear → Optimal → Compute → Optimal → Linear
TestCaseConfig TC114_PrePost_LinearOptimalLinear_NV12() {
    return createTransferTest("TC114_PrePost_LinearOptimalLinear_NV12", 
                             TestFormat::NV12, true, true,
                             TilingMode::Linear, TilingMode::Linear);
}

TestCaseConfig TC115_PrePost_LinearOptimalLinear_P010() {
    return createTransferTest("TC115_PrePost_LinearOptimalLinear_P010", 
                             TestFormat::P010, true, true,
                             TilingMode::Linear, TilingMode::Linear);
}

// Transfer with format conversion (RGBA input with post-transfer)
TestCaseConfig TC116_RGBA2NV12_PostTransfer_ToBuffer() {
    TestCaseConfig config;
    config.name = "TC116_RGBA2NV12_PostTransfer_ToBuffer";
    config.filterType = VulkanFilterYuvCompute::RGBA2YCBCR;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    config.filterFlags = VulkanFilterYuvCompute::FLAG_POST_TRANSFER;
    
    config.inputs.push_back({
        .format = TestFormat::RGBA8,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    // Primary output: optimal image
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

// Transfer-only operations (XFER filter types)
TestCaseConfig TC117_XFER_ImageToBuffer_NV12() {
    TestCaseConfig config;
    config.name = "TC117_XFER_ImageToBuffer_NV12";
    config.filterType = VulkanFilterYuvCompute::XFER_IMAGE_TO_BUFFER;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

TestCaseConfig TC118_XFER_BufferToImage_NV12() {
    TestCaseConfig config;
    config.name = "TC118_XFER_BufferToImage_NV12";
    config.filterType = VulkanFilterYuvCompute::XFER_BUFFER_TO_IMAGE;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Buffer,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

TestCaseConfig TC119_XFER_ImageToImage_LinearToOptimal() {
    TestCaseConfig config;
    config.name = "TC119_XFER_ImageToImage_LinearToOptimal";
    config.filterType = VulkanFilterYuvCompute::XFER_IMAGE_TO_IMAGE;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

TestCaseConfig TC120_XFER_ImageToImage_OptimalToLinear() {
    TestCaseConfig config;
    config.name = "TC120_XFER_ImageToImage_OptimalToLinear";
    config.filterType = VulkanFilterYuvCompute::XFER_IMAGE_TO_IMAGE;
    config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    
    config.inputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Optimal,
        .width = 1920, .height = 1080,
        .generateTestPattern = true, .validateOutput = false
    });
    
    config.outputs.push_back({
        .format = TestFormat::NV12,
        .resourceType = ResourceType::Image,
        .tiling = TilingMode::Linear,
        .width = 1920, .height = 1080,
        .generateTestPattern = false, .validateOutput = true
    });
    
    return config;
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TestCaseConfig TC100_Small_Resolution_64x64() {
    return createRGBA2YCbCr("TC100_Small_Resolution_64x64", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 64, 64);
}

TestCaseConfig TC101_Odd_Resolution_1921x1081() {
    return createRGBA2YCbCr("TC101_Odd_Resolution_1921x1081", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1921, 1081);
}

TestCaseConfig TC102_4K_Resolution_3840x2160() {
    return createRGBA2YCbCr("TC102_4K_Resolution_3840x2160", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 3840, 2160);
}

TestCaseConfig TC103_8K_Resolution_7680x4320() {
    return createRGBA2YCbCr("TC103_8K_Resolution_7680x4320", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 7680, 4320);
}

TestCaseConfig TC104_Minimum_Resolution_2x2() {
    return createRGBA2YCbCr("TC104_Minimum_Resolution_2x2", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 2, 2);
}

// =============================================================================
// Test Set Getters
// =============================================================================

std::vector<TestCaseConfig> getAllStandardTests() {
    return {
        // RGBA to YCbCr (7 formats - Y410 disabled: packed format needs special handling)
        TC001_RGBA_to_NV12(),
        TC002_RGBA_to_P010(),
        TC003_RGBA_to_P012(),
        TC004_RGBA_to_I420(),
        TC005_RGBA_to_NV16(),
        TC006_RGBA_to_P210(),
        TC007_RGBA_to_YUV444(),
        // TC008_RGBA_to_Y410(),  // Disabled: Y410 is packed format, needs special shader
        
        // YCbCr to RGBA (disabled: shader generation bug in YCBCR2RGBA)
        // TC010_NV12_to_RGBA(),
        // TC011_P010_to_RGBA(),
        // TC012_P012_to_RGBA(),
        // TC013_I420_to_RGBA(),
        // TC014_NV16_to_RGBA(),
        // TC015_P210_to_RGBA(),
        // TC016_YUV444_to_RGBA(),
        // TC017_Y410_to_RGBA(),
        
        // Color primaries (BT.601, BT.709, BT.2020)
        TC020_RGBA_to_NV12_BT601(),
        TC021_RGBA_to_NV12_BT709(),
        TC022_RGBA_to_NV12_BT2020(),
        TC023_RGBA_to_P010_BT601(),
        TC024_RGBA_to_P010_BT709(),
        TC025_RGBA_to_P010_BT2020(),
        
        // Range (Full vs Limited)
        TC030_RGBA_to_NV12_FullRange(),
        TC031_RGBA_to_NV12_LimitedRange(),
        TC032_RGBA_to_P010_FullRange(),
        TC033_RGBA_to_P010_LimitedRange(),
        
        // YCbCr Copy
        TC040_YCbCrCopy_NV12(),
        TC041_YCbCrCopy_P010(),
        TC042_YCbCrCopy_I420(),
        TC043_YCbCrCopy_NV16(),
        TC044_YCbCrCopy_YUV444(),
        
        // YCbCr Clear
        TC050_YCbCrClear_NV12(),
        TC051_YCbCrClear_P010(),
        
        // YCbCr format conversion
        TC060_NV12_to_I420(),
        TC061_I420_to_NV12(),
        TC062_NV12_to_NV16(),
        TC063_NV12_to_YUV444(),
        TC064_P010_to_NV12(),
        TC065_NV12_to_P010(),
        
        // Buffer I/O (disabled: not yet implemented in filter execution)
        // TC070_RGBABuffer_to_NV12Image(),
        // TC071_RGBAImage_to_NV12Buffer(),
        // TC072_RGBABuffer_to_NV12Buffer(),
        // TC073_NV12Buffer_to_RGBAImage(),
        // TC074_NV12Image_to_RGBABuffer(),
        // TC075_RGBABuffer_to_P010Buffer(),
        // TC076_P010Buffer_to_RGBABuffer(),
        
        // Linear tiling
        TC080_RGBA_to_NV12_Linear(),
        TC081_RGBA_to_P010_Linear(),
        TC082_Linear_NV12_to_Optimal_NV12(),
        TC083_Optimal_NV12_to_Linear_NV12(),
        
        // Multi-output (future)
        // TC090_Dual_Output_Optimal_Linear(),
        // TC091_Triple_Output_with_Subsampled(),
        
        // Edge cases
        TC100_Small_Resolution_64x64(),
        TC101_Odd_Resolution_1921x1081(),
        TC102_4K_Resolution_3840x2160(),
        // TC103_8K_Resolution_7680x4320(),  // May exceed GPU memory
        TC104_Minimum_Resolution_2x2(),
    };
}

std::vector<TestCaseConfig> getSmokeTests() {
    return {
        // One from each major category
        TC001_RGBA_to_NV12(),       // 8-bit 4:2:0
        TC002_RGBA_to_P010(),       // 10-bit 4:2:0
        TC005_RGBA_to_NV16(),       // 8-bit 4:2:2
        TC007_RGBA_to_YUV444(),     // 8-bit 4:4:4
        // TC010_NV12_to_RGBA(),    // YCbCr to RGBA - disabled: shader generation bug in YCBCR2RGBA
        TC040_YCbCrCopy_NV12(),     // Copy
        TC050_YCbCrClear_NV12(),    // Clear
        // TC070_RGBABuffer_to_NV12Image(),  // Buffer I/O - not implemented yet
        TC100_Small_Resolution_64x64(),   // Edge case
    };
}

std::vector<TestCaseConfig> getRGBA2YCbCrTests() {
    return {
        TC001_RGBA_to_NV12(),
        TC002_RGBA_to_P010(),
        TC003_RGBA_to_P012(),
        TC004_RGBA_to_I420(),
        TC005_RGBA_to_NV16(),
        TC006_RGBA_to_P210(),
        TC007_RGBA_to_YUV444(),
        TC008_RGBA_to_Y410(),
    };
}

std::vector<TestCaseConfig> getYCbCr2RGBATests() {
    return {
        TC010_NV12_to_RGBA(),
        TC011_P010_to_RGBA(),
        TC012_P012_to_RGBA(),
        TC013_I420_to_RGBA(),
        TC014_NV16_to_RGBA(),
        TC015_P210_to_RGBA(),
        TC016_YUV444_to_RGBA(),
        TC017_Y410_to_RGBA(),
    };
}

std::vector<TestCaseConfig> get8BitTests() {
    return {
        TC001_RGBA_to_NV12(),
        TC004_RGBA_to_I420(),
        TC005_RGBA_to_NV16(),
        TC007_RGBA_to_YUV444(),
        TC010_NV12_to_RGBA(),
        TC013_I420_to_RGBA(),
        TC014_NV16_to_RGBA(),
        TC016_YUV444_to_RGBA(),
    };
}

std::vector<TestCaseConfig> get10BitTests() {
    return {
        TC002_RGBA_to_P010(),
        TC006_RGBA_to_P210(),
        TC008_RGBA_to_Y410(),
        TC011_P010_to_RGBA(),
        TC015_P210_to_RGBA(),
        TC017_Y410_to_RGBA(),
    };
}

std::vector<TestCaseConfig> get12BitTests() {
    return {
        TC003_RGBA_to_P012(),
        TC012_P012_to_RGBA(),
    };
}

std::vector<TestCaseConfig> getBufferIOTests() {
    return {
        TC070_RGBABuffer_to_NV12Image(),
        TC071_RGBAImage_to_NV12Buffer(),
        TC072_RGBABuffer_to_NV12Buffer(),
        TC073_NV12Buffer_to_RGBAImage(),
        TC074_NV12Image_to_RGBABuffer(),
        TC075_RGBABuffer_to_P010Buffer(),
        TC076_P010Buffer_to_RGBABuffer(),
    };
}

std::vector<TestCaseConfig> getColorPrimariesTests() {
    return {
        TC020_RGBA_to_NV12_BT601(),
        TC021_RGBA_to_NV12_BT709(),
        TC022_RGBA_to_NV12_BT2020(),
        TC023_RGBA_to_P010_BT601(),
        TC024_RGBA_to_P010_BT709(),
        TC025_RGBA_to_P010_BT2020(),
    };
}

std::vector<TestCaseConfig> getTransferTests() {
    return {
        // Pre-transfer tests (linear → optimal before compute)
        TC110_PreTransfer_LinearToOptimal_NV12(),
        TC111_PreTransfer_LinearToOptimal_P010(),
        
        // Post-transfer tests (optimal → linear after compute)
        TC112_PostTransfer_OptimalToLinear_NV12(),
        TC113_PostTransfer_OptimalToLinear_P010(),
        
        // Combined pre+post transfer tests
        TC114_PrePost_LinearOptimalLinear_NV12(),
        TC115_PrePost_LinearOptimalLinear_P010(),
        
        // RGBA conversion with post-transfer
        TC116_RGBA2NV12_PostTransfer_ToBuffer(),
        
        // Transfer-only operations (XFER filter types)
        TC117_XFER_ImageToBuffer_NV12(),
        TC118_XFER_BufferToImage_NV12(),
        TC119_XFER_ImageToImage_LinearToOptimal(),
        TC120_XFER_ImageToImage_OptimalToLinear(),
    };
}

// =============================================================================
// Regression Tests (verifying bug fixes)
// =============================================================================

TestCaseConfig TC200_Regression_BT2020_NV12() {
    // Regression test for BT.2020 color primaries bug
    // Issue: BT.2020 was incorrectly mapped to BT.709 coefficients
    // Fixed in: pattern.cpp, VulkanFilterYuvCompute.cpp, ycbcr_utils.h
    return createRGBA2YCbCr("TC200_Regression_BT2020_NV12", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1920, 1080);
}

TestCaseConfig TC201_Regression_BT2020_P010() {
    // Regression test for BT.2020 with 10-bit format
    return createRGBA2YCbCr("TC201_Regression_BT2020_P010", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1920, 1080);
}

TestCaseConfig TC202_Regression_BT2020_Limited() {
    // BT.2020 with limited range (HDR content use case)
    return createRGBA2YCbCr("TC202_Regression_BT2020_Limited", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_NARROW, 3840, 2160);
}

// =============================================================================
// Production Validation Tests
// =============================================================================

TestCaseConfig TC210_Production_HD_NV12_BT709() {
    // Standard HD production test: 1080p NV12 with BT.709
    return createRGBA2YCbCr("TC210_Production_HD_NV12_BT709", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1920, 1080);
}

TestCaseConfig TC211_Production_HD_P010_BT709() {
    // HD 10-bit production test
    return createRGBA2YCbCr("TC211_Production_HD_P010_BT709", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1920, 1080);
}

TestCaseConfig TC212_Production_4K_NV12_BT2020() {
    // 4K HDR production test: 4K with BT.2020
    return createRGBA2YCbCr("TC212_Production_4K_NV12_BT2020", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 3840, 2160);
}

TestCaseConfig TC213_Production_4K_P010_BT2020() {
    // 4K HDR 10-bit production test
    return createRGBA2YCbCr("TC213_Production_4K_P010_BT2020", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 3840, 2160);
}

TestCaseConfig TC214_Production_SD_NV12_BT601() {
    // SD production test: 480p with BT.601
    return createRGBA2YCbCr("TC214_Production_SD_NV12_BT601", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 720, 480);
}

TestCaseConfig TC215_Production_720p_NV12_BT709() {
    // 720p production test
    return createRGBA2YCbCr("TC215_Production_720p_NV12_BT709", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 1280, 720);
}

// =============================================================================
// Broadcast Standard Tests (Limited Range)
// =============================================================================

TestCaseConfig TC220_Broadcast_HD_Limited() {
    // Broadcast HD: limited range for TV output
    return createRGBA2YCbCr("TC220_Broadcast_HD_Limited", TestFormat::NV12,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                            VK_SAMPLER_YCBCR_RANGE_ITU_NARROW, 1920, 1080);
}

TestCaseConfig TC221_Broadcast_4K_Limited() {
    // Broadcast 4K: limited range
    return createRGBA2YCbCr("TC221_Broadcast_4K_Limited", TestFormat::P010,
                            VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
                            VK_SAMPLER_YCBCR_RANGE_ITU_NARROW, 3840, 2160);
}

// =============================================================================
// Get functions for new test categories
// =============================================================================

std::vector<TestCaseConfig> getRegressionTests() {
    return {
        TC200_Regression_BT2020_NV12(),
        TC201_Regression_BT2020_P010(),
        TC202_Regression_BT2020_Limited(),
    };
}

std::vector<TestCaseConfig> getProductionTests() {
    return {
        TC210_Production_HD_NV12_BT709(),
        TC211_Production_HD_P010_BT709(),
        TC212_Production_4K_NV12_BT2020(),
        TC213_Production_4K_P010_BT2020(),
        TC214_Production_SD_NV12_BT601(),
        TC215_Production_720p_NV12_BT709(),
        TC220_Broadcast_HD_Limited(),
        TC221_Broadcast_4K_Limited(),
    };
}

} // namespace TestCases
} // namespace vkfilter_test
