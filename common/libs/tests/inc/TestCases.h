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

#ifndef _FILTER_TEST_CASES_H_
#define _FILTER_TEST_CASES_H_

#include "FilterTestApp.h"

namespace vkfilter_test {

/**
 * @brief Standard test case definitions for VulkanFilterYuvCompute
 * 
 * Supported YCbCr Formats:
 * Value | Format  | Subsampling | Bit Depth | Description
 * ------|---------|-------------|-----------|-----------------------------
 * 0     | NV12    | 4:2:0       | 8-bit     | 2-plane (Y + interleaved UV)
 * 1     | P010    | 4:2:0       | 10-bit    | 2-plane, 16-bit storage
 * 2     | P012    | 4:2:0       | 12-bit    | 2-plane, 16-bit storage
 * 3     | I420    | 4:2:0       | 8-bit     | 3-plane (Y + U + V)
 * 4     | NV16    | 4:2:2       | 8-bit     | 2-plane
 * 5     | P210    | 4:2:2       | 10-bit    | 2-plane, 16-bit storage
 * 6     | YUV444  | 4:4:4       | 8-bit     | 3-plane
 * 7     | Y410    | 4:4:4       | 10-bit    | Packed AVYU
 * 
 * Test categories:
 * - RGBA to YCbCr conversion (all formats)
 * - YCbCr to RGBA conversion (all formats)
 * - YCbCr copy (same format)
 * - YCbCr format conversion (different YCbCr formats)
 * - Buffer I/O (image↔buffer)
 * - Linear/Optimal tiling
 * - Color primaries (BT.601, BT.709, BT.2020)
 * - Range (Full, Limited)
 * - Edge cases (small, odd, 4K resolution)
 */
namespace TestCases {

// =============================================================================
// RGBA to YCbCr Conversion Tests (All 8 YCbCr formats)
// =============================================================================

// 4:2:0 formats
TestCaseConfig TC001_RGBA_to_NV12();        // 8-bit 4:2:0 2-plane
TestCaseConfig TC002_RGBA_to_P010();        // 10-bit 4:2:0 2-plane
TestCaseConfig TC003_RGBA_to_P012();        // 12-bit 4:2:0 2-plane
TestCaseConfig TC004_RGBA_to_I420();        // 8-bit 4:2:0 3-plane

// 4:2:2 formats
TestCaseConfig TC005_RGBA_to_NV16();        // 8-bit 4:2:2 2-plane
TestCaseConfig TC006_RGBA_to_P210();        // 10-bit 4:2:2 2-plane

// 4:4:4 formats
TestCaseConfig TC007_RGBA_to_YUV444();      // 8-bit 4:4:4 3-plane
TestCaseConfig TC008_RGBA_to_Y410();        // 10-bit 4:4:4 packed

// =============================================================================
// YCbCr to RGBA Conversion Tests (All 8 YCbCr formats)
// =============================================================================

// 4:2:0 formats
TestCaseConfig TC010_NV12_to_RGBA();        // 8-bit 4:2:0 2-plane
TestCaseConfig TC011_P010_to_RGBA();        // 10-bit 4:2:0 2-plane
TestCaseConfig TC012_P012_to_RGBA();        // 12-bit 4:2:0 2-plane
TestCaseConfig TC013_I420_to_RGBA();        // 8-bit 4:2:0 3-plane

// 4:2:2 formats
TestCaseConfig TC014_NV16_to_RGBA();        // 8-bit 4:2:2 2-plane
TestCaseConfig TC015_P210_to_RGBA();        // 10-bit 4:2:2 2-plane

// 4:4:4 formats
TestCaseConfig TC016_YUV444_to_RGBA();      // 8-bit 4:4:4 3-plane
TestCaseConfig TC017_Y410_to_RGBA();        // 10-bit 4:4:4 packed

// =============================================================================
// Color Primaries Tests (BT.601, BT.709, BT.2020)
// =============================================================================

TestCaseConfig TC020_RGBA_to_NV12_BT601();
TestCaseConfig TC021_RGBA_to_NV12_BT709();
TestCaseConfig TC022_RGBA_to_NV12_BT2020();

TestCaseConfig TC023_RGBA_to_P010_BT601();
TestCaseConfig TC024_RGBA_to_P010_BT709();
TestCaseConfig TC025_RGBA_to_P010_BT2020();

// =============================================================================
// Range Tests (Full vs Limited)
// =============================================================================

TestCaseConfig TC030_RGBA_to_NV12_FullRange();
TestCaseConfig TC031_RGBA_to_NV12_LimitedRange();
TestCaseConfig TC032_RGBA_to_P010_FullRange();
TestCaseConfig TC033_RGBA_to_P010_LimitedRange();

// =============================================================================
// YCbCr Copy Tests (Same format in → out)
// =============================================================================

TestCaseConfig TC040_YCbCrCopy_NV12();
TestCaseConfig TC041_YCbCrCopy_P010();
TestCaseConfig TC042_YCbCrCopy_I420();
TestCaseConfig TC043_YCbCrCopy_NV16();
TestCaseConfig TC044_YCbCrCopy_YUV444();

// =============================================================================
// YCbCr Clear Tests
// =============================================================================

TestCaseConfig TC050_YCbCrClear_NV12();
TestCaseConfig TC051_YCbCrClear_P010();

// =============================================================================
// YCbCr Format Conversion Tests (Different YCbCr in → out)
// =============================================================================

TestCaseConfig TC060_NV12_to_I420();        // 2-plane to 3-plane
TestCaseConfig TC061_I420_to_NV12();        // 3-plane to 2-plane
TestCaseConfig TC062_NV12_to_NV16();        // 4:2:0 to 4:2:2
TestCaseConfig TC063_NV12_to_YUV444();      // 4:2:0 to 4:4:4
TestCaseConfig TC064_P010_to_NV12();        // 10-bit to 8-bit
TestCaseConfig TC065_NV12_to_P010();        // 8-bit to 10-bit

// =============================================================================
// Buffer I/O Tests
// =============================================================================

// RGBA buffer tests
TestCaseConfig TC070_RGBABuffer_to_NV12Image();
TestCaseConfig TC071_RGBAImage_to_NV12Buffer();
TestCaseConfig TC072_RGBABuffer_to_NV12Buffer();

// YCbCr buffer tests
TestCaseConfig TC073_NV12Buffer_to_RGBAImage();
TestCaseConfig TC074_NV12Image_to_RGBABuffer();

// Buffer-to-buffer with format conversion
TestCaseConfig TC075_RGBABuffer_to_P010Buffer();
TestCaseConfig TC076_P010Buffer_to_RGBABuffer();

// =============================================================================
// Linear Tiling Tests
// =============================================================================

TestCaseConfig TC080_RGBA_to_NV12_Linear();
TestCaseConfig TC081_RGBA_to_P010_Linear();
TestCaseConfig TC082_Linear_NV12_to_Optimal_NV12();
TestCaseConfig TC083_Optimal_NV12_to_Linear_NV12();

// =============================================================================
// Multi-Output Tests (Future - for flexible I/O)
// =============================================================================

TestCaseConfig TC090_Dual_Output_Optimal_Linear();
TestCaseConfig TC091_Triple_Output_with_Subsampled();

// =============================================================================
// Edge Case Tests
// =============================================================================

TestCaseConfig TC100_Small_Resolution_64x64();
TestCaseConfig TC101_Odd_Resolution_1921x1081();
TestCaseConfig TC102_4K_Resolution_3840x2160();
TestCaseConfig TC103_8K_Resolution_7680x4320();
TestCaseConfig TC104_Minimum_Resolution_2x2();

// =============================================================================
// Transfer Operation Tests (Pre/Post Transfer scenarios)
// =============================================================================

// Pre-transfer: Linear source copied to optimal before compute
TestCaseConfig TC110_PreTransfer_LinearToOptimal_NV12();
TestCaseConfig TC111_PreTransfer_LinearToOptimal_P010();

// Post-transfer: Optimal output copied to linear after compute
TestCaseConfig TC112_PostTransfer_OptimalToLinear_NV12();
TestCaseConfig TC113_PostTransfer_OptimalToLinear_P010();

// Both pre and post transfer: Linear → Optimal → Compute → Optimal → Linear
TestCaseConfig TC114_PrePost_LinearOptimalLinear_NV12();
TestCaseConfig TC115_PrePost_LinearOptimalLinear_P010();

// Transfer with format conversion
TestCaseConfig TC116_RGBA2NV12_PostTransfer_ToBuffer();

// Transfer-only operations (XFER filter types)
TestCaseConfig TC117_XFER_ImageToBuffer_NV12();
TestCaseConfig TC118_XFER_BufferToImage_NV12();
TestCaseConfig TC119_XFER_ImageToImage_LinearToOptimal();
TestCaseConfig TC120_XFER_ImageToImage_OptimalToLinear();

// =============================================================================
// Test Set Getters
// =============================================================================

/**
 * @brief Get all standard test cases (comprehensive)
 */
std::vector<TestCaseConfig> getAllStandardTests();

/**
 * @brief Get quick smoke test cases (one per category)
 */
std::vector<TestCaseConfig> getSmokeTests();

/**
 * @brief Get RGBA to YCbCr tests only
 */
std::vector<TestCaseConfig> getRGBA2YCbCrTests();

/**
 * @brief Get YCbCr to RGBA tests only
 */
std::vector<TestCaseConfig> getYCbCr2RGBATests();

/**
 * @brief Get all 8-bit format tests
 */
std::vector<TestCaseConfig> get8BitTests();

/**
 * @brief Get all 10-bit format tests
 */
std::vector<TestCaseConfig> get10BitTests();

/**
 * @brief Get all 12-bit format tests
 */
std::vector<TestCaseConfig> get12BitTests();

/**
 * @brief Get buffer I/O tests only
 */
std::vector<TestCaseConfig> getBufferIOTests();

/**
 * @brief Get color primaries tests only
 */
std::vector<TestCaseConfig> getColorPrimariesTests();

/**
 * @brief Get transfer operation tests (pre/post transfer, transfer-only)
 */
std::vector<TestCaseConfig> getTransferTests();

// =============================================================================
// Regression Tests (verifying bug fixes)
// =============================================================================

/**
 * @brief Regression test for BT.2020 color primaries bug
 * Issue: BT.2020 was incorrectly mapped to BT.709 coefficients
 * Fixed in: pattern.cpp, VulkanFilterYuvCompute.cpp, ycbcr_utils.h
 */
TestCaseConfig TC200_Regression_BT2020_NV12();
TestCaseConfig TC201_Regression_BT2020_P010();
TestCaseConfig TC202_Regression_BT2020_Limited();

// =============================================================================
// Production Validation Tests
// =============================================================================

TestCaseConfig TC210_Production_HD_NV12_BT709();      // 1080p NV12 BT.709
TestCaseConfig TC211_Production_HD_P010_BT709();      // 1080p P010 BT.709
TestCaseConfig TC212_Production_4K_NV12_BT2020();     // 4K NV12 BT.2020
TestCaseConfig TC213_Production_4K_P010_BT2020();     // 4K P010 BT.2020 (HDR)
TestCaseConfig TC214_Production_SD_NV12_BT601();      // 480p NV12 BT.601
TestCaseConfig TC215_Production_720p_NV12_BT709();    // 720p NV12 BT.709
TestCaseConfig TC220_Broadcast_HD_Limited();          // 1080p limited range
TestCaseConfig TC221_Broadcast_4K_Limited();          // 4K limited range

/**
 * @brief Get regression tests (verify bug fixes)
 */
std::vector<TestCaseConfig> getRegressionTests();

/**
 * @brief Get production validation tests (real-world use cases)
 */
std::vector<TestCaseConfig> getProductionTests();

} // namespace TestCases

} // namespace vkfilter_test

#endif /* _FILTER_TEST_CASES_H_ */
