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

/**
 * @file ColorConversion.h
 * @brief CPU reference implementation for RGB↔YCbCr color space conversion
 * 
 * This module provides accurate CPU implementations of color conversion
 * for validating GPU filter outputs. Supports:
 * - BT.601, BT.709, BT.2020 color primaries
 * - Full range and limited (narrow) range
 * - 8-bit, 10-bit, 12-bit bit depths
 * - Various chroma subsampling (4:4:4, 4:2:2, 4:2:0)
 */

#ifndef _COLOR_CONVERSION_H_
#define _COLOR_CONVERSION_H_

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkfilter_test {

// =============================================================================
// Color Primaries and Range Constants
// =============================================================================

/**
 * @brief Color primaries standard
 */
enum class ColorPrimaries {
    BT601,      // ITU-R BT.601 (SD video)
    BT709,      // ITU-R BT.709 (HD video)
    BT2020,     // ITU-R BT.2020 (UHD/HDR video)
};

/**
 * @brief Color range
 */
enum class ColorRange {
    Full,       // Full range [0-255] for 8-bit
    Limited,    // Limited range [16-235]/[16-240] for 8-bit (narrow/studio)
};

/**
 * @brief Get Vulkan YCbCr model from ColorPrimaries
 */
inline VkSamplerYcbcrModelConversion toVkYcbcrModel(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::BT601:  return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        case ColorPrimaries::BT709:  return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        case ColorPrimaries::BT2020: return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
        default: return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    }
}

/**
 * @brief Get Vulkan YCbCr range from ColorRange
 */
inline VkSamplerYcbcrRange toVkYcbcrRange(ColorRange range) {
    return (range == ColorRange::Limited) ? 
           VK_SAMPLER_YCBCR_RANGE_ITU_NARROW : 
           VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
}

/**
 * @brief Get ColorPrimaries from Vulkan model
 */
inline ColorPrimaries fromVkYcbcrModel(VkSamplerYcbcrModelConversion model) {
    switch (model) {
        case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:  return ColorPrimaries::BT601;
        case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709:  return ColorPrimaries::BT709;
        case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020: return ColorPrimaries::BT2020;
        default: return ColorPrimaries::BT709;
    }
}

/**
 * @brief Get ColorRange from Vulkan range
 */
inline ColorRange fromVkYcbcrRange(VkSamplerYcbcrRange range) {
    return (range == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW) ? 
           ColorRange::Limited : ColorRange::Full;
}

// =============================================================================
// Color Conversion Matrix
// =============================================================================

/**
 * @brief Color conversion coefficients for a specific standard
 * 
 * RGB to YCbCr:
 *   Y  = Kr*R + Kg*G + Kb*B
 *   Cb = (B - Y) / (2*(1-Kb))
 *   Cr = (R - Y) / (2*(1-Kr))
 * 
 * YCbCr to RGB:
 *   R = Y + 2*(1-Kr)*Cr
 *   G = Y - 2*Kb*(1-Kb)/Kg*Cb - 2*Kr*(1-Kr)/Kg*Cr
 *   B = Y + 2*(1-Kb)*Cb
 */
struct ColorCoefficients {
    double Kr;  // Red coefficient for Y
    double Kb;  // Blue coefficient for Y
    double Kg;  // Green coefficient (calculated as 1 - Kr - Kb)
    
    // Derived coefficients for RGB to YCbCr
    double CbScale;  // = 0.5 / (1 - Kb)
    double CrScale;  // = 0.5 / (1 - Kr)
    
    // Derived coefficients for YCbCr to RGB
    double CbToB;    // = 2 * (1 - Kb)
    double CrToR;    // = 2 * (1 - Kr)
    double CbToG;    // = -2 * Kb * (1 - Kb) / Kg
    double CrToG;    // = -2 * Kr * (1 - Kr) / Kg
    
    ColorCoefficients(double kr, double kb) 
        : Kr(kr), Kb(kb), Kg(1.0 - kr - kb)
    {
        CbScale = 0.5 / (1.0 - Kb);
        CrScale = 0.5 / (1.0 - Kr);
        CbToB = 2.0 * (1.0 - Kb);
        CrToR = 2.0 * (1.0 - Kr);
        CbToG = -2.0 * Kb * (1.0 - Kb) / Kg;
        CrToG = -2.0 * Kr * (1.0 - Kr) / Kg;
    }
};

/**
 * @brief Get color coefficients for a standard
 */
inline ColorCoefficients getColorCoefficients(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::BT601:
            // ITU-R BT.601: Kr=0.299, Kb=0.114
            return ColorCoefficients(0.299, 0.114);
        case ColorPrimaries::BT709:
            // ITU-R BT.709: Kr=0.2126, Kb=0.0722
            return ColorCoefficients(0.2126, 0.0722);
        case ColorPrimaries::BT2020:
            // ITU-R BT.2020: Kr=0.2627, Kb=0.0593
            return ColorCoefficients(0.2627, 0.0593);
        default:
            return ColorCoefficients(0.2126, 0.0722);  // Default to BT.709
    }
}

// =============================================================================
// Range Conversion Parameters
// =============================================================================

/**
 * @brief Range parameters for a specific bit depth
 */
struct RangeParams {
    uint32_t bitDepth;
    uint32_t maxValue;      // Max value for bit depth (e.g., 255 for 8-bit)
    
    // Limited range parameters
    uint32_t yBlack;        // Y black level
    uint32_t yWhite;        // Y white level
    uint32_t cZero;         // CbCr zero level (mid-point)
    uint32_t cScale;        // CbCr range
    
    RangeParams(uint32_t bits) : bitDepth(bits) {
        maxValue = (1 << bits) - 1;
        
        // Scale limited range parameters based on bit depth
        double scale = static_cast<double>(1 << (bits - 8));
        yBlack = static_cast<uint32_t>(16.0 * scale);
        yWhite = static_cast<uint32_t>(235.0 * scale);
        cZero  = static_cast<uint32_t>(128.0 * scale);
        cScale = static_cast<uint32_t>(224.0 * scale);
    }
};

// =============================================================================
// Pixel Types
// =============================================================================

/**
 * @brief RGB pixel (normalized 0.0-1.0)
 */
struct RGBPixel {
    double r, g, b;
    
    RGBPixel() : r(0), g(0), b(0) {}
    RGBPixel(double r_, double g_, double b_) : r(r_), g(g_), b(b_) {}
    
    // Clamp to valid range
    void clamp() {
        r = std::max(0.0, std::min(1.0, r));
        g = std::max(0.0, std::min(1.0, g));
        b = std::max(0.0, std::min(1.0, b));
    }
};

/**
 * @brief YCbCr pixel (normalized: Y in 0-1, Cb/Cr in -0.5 to 0.5)
 */
struct YCbCrPixel {
    double y, cb, cr;
    
    YCbCrPixel() : y(0), cb(0), cr(0) {}
    YCbCrPixel(double y_, double cb_, double cr_) : y(y_), cb(cb_), cr(cr_) {}
    
    // Clamp to valid range
    void clamp() {
        y  = std::max(0.0, std::min(1.0, y));
        cb = std::max(-0.5, std::min(0.5, cb));
        cr = std::max(-0.5, std::min(0.5, cr));
    }
};

// =============================================================================
// Color Conversion Functions
// =============================================================================

/**
 * @brief Convert RGB to YCbCr (normalized values)
 * @param rgb Input RGB (0-1 range)
 * @param primaries Color primaries standard
 * @return YCbCr (Y: 0-1, Cb/Cr: -0.5 to 0.5)
 */
inline YCbCrPixel rgbToYCbCr(const RGBPixel& rgb, ColorPrimaries primaries) {
    const ColorCoefficients c = getColorCoefficients(primaries);
    
    YCbCrPixel ycbcr;
    ycbcr.y  = c.Kr * rgb.r + c.Kg * rgb.g + c.Kb * rgb.b;
    ycbcr.cb = (rgb.b - ycbcr.y) * c.CbScale;
    ycbcr.cr = (rgb.r - ycbcr.y) * c.CrScale;
    
    return ycbcr;
}

/**
 * @brief Convert YCbCr to RGB (normalized values)
 * @param ycbcr Input YCbCr (Y: 0-1, Cb/Cr: -0.5 to 0.5)
 * @param primaries Color primaries standard
 * @return RGB (0-1 range)
 */
inline RGBPixel ycbcrToRgb(const YCbCrPixel& ycbcr, ColorPrimaries primaries) {
    const ColorCoefficients c = getColorCoefficients(primaries);
    
    RGBPixel rgb;
    rgb.r = ycbcr.y + c.CrToR * ycbcr.cr;
    rgb.g = ycbcr.y + c.CbToG * ycbcr.cb + c.CrToG * ycbcr.cr;
    rgb.b = ycbcr.y + c.CbToB * ycbcr.cb;
    rgb.clamp();
    
    return rgb;
}

// =============================================================================
// Integer Conversion Functions (with range handling)
// =============================================================================

/**
 * @brief Convert 8-bit RGB to YCbCr with range handling
 * @param r, g, b Input RGB (0-255)
 * @param primaries Color primaries
 * @param range Color range (full or limited)
 * @param y, cb, cr Output YCbCr values
 */
void rgbToYCbCr8(uint8_t r, uint8_t g, uint8_t b,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& y, uint8_t& cb, uint8_t& cr);

/**
 * @brief Convert 8-bit YCbCr to RGB with range handling
 * @param y, cb, cr Input YCbCr values
 * @param primaries Color primaries
 * @param range Color range (full or limited)
 * @param r, g, b Output RGB (0-255)
 */
void ycbcrToRgb8(uint8_t y, uint8_t cb, uint8_t cr,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& r, uint8_t& g, uint8_t& b);

/**
 * @brief Convert 16-bit RGB to YCbCr with range handling (for 10/12-bit content)
 * @param r, g, b Input RGB (0-maxVal based on bit depth)
 * @param bitDepth Actual bit depth (10 or 12, stored in 16-bit)
 * @param primaries Color primaries
 * @param range Color range (full or limited)
 * @param y, cb, cr Output YCbCr values (MSB-aligned in 16-bit)
 */
void rgbToYCbCr16(uint16_t r, uint16_t g, uint16_t b,
                  uint32_t bitDepth,
                  ColorPrimaries primaries, ColorRange range,
                  uint16_t& y, uint16_t& cb, uint16_t& cr);

/**
 * @brief Convert 16-bit YCbCr to RGB with range handling
 * @param y, cb, cr Input YCbCr values (MSB-aligned in 16-bit)
 * @param bitDepth Actual bit depth (10 or 12)
 * @param primaries Color primaries
 * @param range Color range (full or limited)
 * @param r, g, b Output RGB (scaled to 8-bit)
 */
void ycbcrToRgb16(uint16_t y, uint16_t cb, uint16_t cr,
                  uint32_t bitDepth,
                  ColorPrimaries primaries, ColorRange range,
                  uint8_t& r, uint8_t& g, uint8_t& b);

// =============================================================================
// Test Pattern Generation
// =============================================================================

/**
 * @brief Test pattern types for validation
 */
enum class TestPatternType {
    ColorBars,      // Standard SMPTE color bars
    Gradient,       // Horizontal gradient (black to white)
    Checkerboard,   // Checkerboard pattern
    Ramp,           // Full ramp (all values)
    Solid,          // Solid color (for specific color testing)
    Random,         // Pseudo-random pattern
};

/**
 * @brief Generate RGBA test pattern
 * @param type Pattern type
 * @param width Image width
 * @param height Image height
 * @param data Output buffer (width * height * 4 bytes)
 */
void generateRGBATestPattern(TestPatternType type, 
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t>& data);

/**
 * @brief Generate YCbCr test pattern (NV12 format)
 * @param type Pattern type
 * @param width Image width
 * @param height Image height
 * @param primaries Color primaries for conversion
 * @param range Color range
 * @param yPlane Output Y plane
 * @param uvPlane Output interleaved UV plane
 */
void generateNV12TestPattern(TestPatternType type,
                             uint32_t width, uint32_t height,
                             ColorPrimaries primaries, ColorRange range,
                             std::vector<uint8_t>& yPlane,
                             std::vector<uint8_t>& uvPlane);

// =============================================================================
// Bulk Conversion Functions
// =============================================================================

/**
 * @brief Convert RGBA buffer to NV12 (reference implementation)
 * @param rgba Input RGBA data (width * height * 4 bytes)
 * @param width Image width
 * @param height Image height
 * @param primaries Color primaries
 * @param range Color range
 * @param yPlane Output Y plane
 * @param uvPlane Output interleaved UV plane
 */
void convertRGBAtoNV12(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uvPlane);

/**
 * @brief Convert NV12 to RGBA buffer (reference implementation)
 * @param yPlane Input Y plane
 * @param uvPlane Input interleaved UV plane
 * @param width Image width
 * @param height Image height
 * @param primaries Color primaries
 * @param range Color range
 * @param rgba Output RGBA data
 */
void convertNV12toRGBA(const uint8_t* yPlane,
                       const uint8_t* uvPlane,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& rgba);

/**
 * @brief Convert RGBA buffer to P010 (10-bit NV12)
 */
void convertRGBAtoP010(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint16_t>& yPlane,
                       std::vector<uint16_t>& uvPlane);

/**
 * @brief Convert RGBA buffer to I420 (3-plane)
 */
void convertRGBAtoI420(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uPlane,
                       std::vector<uint8_t>& vPlane);

/**
 * @brief Convert RGBA buffer to NV16 (4:2:2)
 */
void convertRGBAtoNV16(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uvPlane);

/**
 * @brief Convert RGBA buffer to YUV444 (3-plane 4:4:4)
 */
void convertRGBAtoYUV444(const uint8_t* rgba,
                         uint32_t width, uint32_t height,
                         ColorPrimaries primaries, ColorRange range,
                         std::vector<uint8_t>& yPlane,
                         std::vector<uint8_t>& uPlane,
                         std::vector<uint8_t>& vPlane);

// =============================================================================
// Validation Functions
// =============================================================================

/**
 * @brief Validation result
 */
struct ValidationResult {
    bool passed{false};
    double psnrY{0.0};
    double psnrCb{0.0};
    double psnrCr{0.0};
    double maxErrorY{0.0};
    double maxErrorCb{0.0};
    double maxErrorCr{0.0};
    uint32_t errorCountY{0};
    uint32_t errorCountCb{0};
    uint32_t errorCountCr{0};
    std::string errorMessage;
};

/**
 * @brief Compare two YCbCr buffers (NV12)
 * @param actual Actual output
 * @param expected Expected reference
 * @param width Image width
 * @param height Image height
 * @param tolerance Maximum allowed per-sample error (0-255 for 8-bit)
 * @return Validation result with PSNR metrics
 */
ValidationResult compareNV12(const uint8_t* actualY, const uint8_t* actualUV,
                             const uint8_t* expectedY, const uint8_t* expectedUV,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance = 2);

/**
 * @brief Compare two RGBA buffers
 */
ValidationResult compareRGBA(const uint8_t* actual, const uint8_t* expected,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance = 2);

/**
 * @brief Calculate PSNR between two buffers
 */
double calculatePSNR(const uint8_t* data1, const uint8_t* data2, 
                     size_t size, uint32_t maxValue = 255);

/**
 * @brief Calculate PSNR for 16-bit data
 */
double calculatePSNR16(const uint16_t* data1, const uint16_t* data2, 
                       size_t size, uint32_t bitDepth);

} // namespace vkfilter_test

#endif /* _COLOR_CONVERSION_H_ */
