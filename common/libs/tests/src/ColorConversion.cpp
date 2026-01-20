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

#include "ColorConversion.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace vkfilter_test {

// =============================================================================
// Integer Conversion Functions
// =============================================================================

void rgbToYCbCr8(uint8_t r, uint8_t g, uint8_t b,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& yOut, uint8_t& cbOut, uint8_t& crOut)
{
    // Normalize RGB to 0-1
    double rNorm = r / 255.0;
    double gNorm = g / 255.0;
    double bNorm = b / 255.0;
    
    // Convert to YCbCr (normalized)
    RGBPixel rgb(rNorm, gNorm, bNorm);
    YCbCrPixel ycbcr = rgbToYCbCr(rgb, primaries);
    
    // Denormalize based on range
    if (range == ColorRange::Limited) {
        // Limited range: Y [16-235], Cb/Cr [16-240]
        yOut  = static_cast<uint8_t>(std::round(ycbcr.y * 219.0 + 16.0));
        cbOut = static_cast<uint8_t>(std::round((ycbcr.cb + 0.5) * 224.0 + 16.0));
        crOut = static_cast<uint8_t>(std::round((ycbcr.cr + 0.5) * 224.0 + 16.0));
    } else {
        // Full range: Y [0-255], Cb/Cr [0-255]
        yOut  = static_cast<uint8_t>(std::round(ycbcr.y * 255.0));
        cbOut = static_cast<uint8_t>(std::round((ycbcr.cb + 0.5) * 255.0));
        crOut = static_cast<uint8_t>(std::round((ycbcr.cr + 0.5) * 255.0));
    }
    
    // Clamp
    yOut  = std::min<uint8_t>(yOut, 255);
    cbOut = std::min<uint8_t>(cbOut, 255);
    crOut = std::min<uint8_t>(crOut, 255);
}

void ycbcrToRgb8(uint8_t y, uint8_t cb, uint8_t cr,
                 ColorPrimaries primaries, ColorRange range,
                 uint8_t& rOut, uint8_t& gOut, uint8_t& bOut)
{
    double yNorm, cbNorm, crNorm;
    
    // Normalize based on range
    if (range == ColorRange::Limited) {
        // Limited range: Y [16-235], Cb/Cr [16-240]
        yNorm  = (y - 16.0) / 219.0;
        cbNorm = ((cb - 16.0) / 224.0) - 0.5;
        crNorm = ((cr - 16.0) / 224.0) - 0.5;
    } else {
        // Full range: Y [0-255], Cb/Cr [0-255]
        yNorm  = y / 255.0;
        cbNorm = (cb / 255.0) - 0.5;
        crNorm = (cr / 255.0) - 0.5;
    }
    
    // Clamp normalized values
    yNorm  = std::max(0.0, std::min(1.0, yNorm));
    cbNorm = std::max(-0.5, std::min(0.5, cbNorm));
    crNorm = std::max(-0.5, std::min(0.5, crNorm));
    
    // Convert to RGB
    YCbCrPixel ycbcr(yNorm, cbNorm, crNorm);
    RGBPixel rgb = ycbcrToRgb(ycbcr, primaries);
    
    // Denormalize and clamp
    rOut = static_cast<uint8_t>(std::round(rgb.r * 255.0));
    gOut = static_cast<uint8_t>(std::round(rgb.g * 255.0));
    bOut = static_cast<uint8_t>(std::round(rgb.b * 255.0));
}

void rgbToYCbCr16(uint16_t r, uint16_t g, uint16_t b,
                  uint32_t bitDepth,
                  ColorPrimaries primaries, ColorRange range,
                  uint16_t& yOut, uint16_t& cbOut, uint16_t& crOut)
{
    // Determine max value and range parameters for bit depth
    uint32_t maxVal = (1 << bitDepth) - 1;
    uint32_t shiftAmount = 16 - bitDepth;  // For MSB alignment in 16-bit storage
    
    // Normalize RGB to 0-1
    double rNorm = static_cast<double>(r) / maxVal;
    double gNorm = static_cast<double>(g) / maxVal;
    double bNorm = static_cast<double>(b) / maxVal;
    
    // Convert to YCbCr (normalized)
    RGBPixel rgb(rNorm, gNorm, bNorm);
    YCbCrPixel ycbcr = rgbToYCbCr(rgb, primaries);
    
    // Get range parameters
    RangeParams rp(bitDepth);
    
    // Denormalize based on range
    uint16_t yVal, cbVal, crVal;
    if (range == ColorRange::Limited) {
        // Limited range
        double yRange = rp.yWhite - rp.yBlack;
        yVal  = static_cast<uint16_t>(std::round(ycbcr.y * yRange + rp.yBlack));
        cbVal = static_cast<uint16_t>(std::round((ycbcr.cb + 0.5) * rp.cScale + rp.yBlack));
        crVal = static_cast<uint16_t>(std::round((ycbcr.cr + 0.5) * rp.cScale + rp.yBlack));
    } else {
        // Full range
        yVal  = static_cast<uint16_t>(std::round(ycbcr.y * maxVal));
        cbVal = static_cast<uint16_t>(std::round((ycbcr.cb + 0.5) * maxVal));
        crVal = static_cast<uint16_t>(std::round((ycbcr.cr + 0.5) * maxVal));
    }
    
    // Clamp and shift to MSB for 16-bit storage
    yOut  = std::min(yVal, static_cast<uint16_t>(maxVal)) << shiftAmount;
    cbOut = std::min(cbVal, static_cast<uint16_t>(maxVal)) << shiftAmount;
    crOut = std::min(crVal, static_cast<uint16_t>(maxVal)) << shiftAmount;
}

void ycbcrToRgb16(uint16_t y, uint16_t cb, uint16_t cr,
                  uint32_t bitDepth,
                  ColorPrimaries primaries, ColorRange range,
                  uint8_t& rOut, uint8_t& gOut, uint8_t& bOut)
{
    // Extract actual bit values from MSB-aligned 16-bit storage
    uint32_t shiftAmount = 16 - bitDepth;
    uint32_t maxVal = (1 << bitDepth) - 1;
    
    uint16_t yVal  = y >> shiftAmount;
    uint16_t cbVal = cb >> shiftAmount;
    uint16_t crVal = cr >> shiftAmount;
    
    // Normalize based on range
    double yNorm, cbNorm, crNorm;
    
    if (range == ColorRange::Limited) {
        RangeParams rp(bitDepth);
        double yRange = rp.yWhite - rp.yBlack;
        yNorm  = (static_cast<double>(yVal) - rp.yBlack) / yRange;
        cbNorm = ((static_cast<double>(cbVal) - rp.yBlack) / rp.cScale) - 0.5;
        crNorm = ((static_cast<double>(crVal) - rp.yBlack) / rp.cScale) - 0.5;
    } else {
        yNorm  = static_cast<double>(yVal) / maxVal;
        cbNorm = (static_cast<double>(cbVal) / maxVal) - 0.5;
        crNorm = (static_cast<double>(crVal) / maxVal) - 0.5;
    }
    
    // Clamp normalized values
    yNorm  = std::max(0.0, std::min(1.0, yNorm));
    cbNorm = std::max(-0.5, std::min(0.5, cbNorm));
    crNorm = std::max(-0.5, std::min(0.5, crNorm));
    
    // Convert to RGB
    YCbCrPixel ycbcr(yNorm, cbNorm, crNorm);
    RGBPixel rgb = ycbcrToRgb(ycbcr, primaries);
    
    // Denormalize and clamp to 8-bit output
    rOut = static_cast<uint8_t>(std::round(rgb.r * 255.0));
    gOut = static_cast<uint8_t>(std::round(rgb.g * 255.0));
    bOut = static_cast<uint8_t>(std::round(rgb.b * 255.0));
}

// =============================================================================
// Test Pattern Generation
// =============================================================================

// SMPTE color bars: White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
static const uint8_t colorBars[8][3] = {
    {235, 235, 235},  // White
    {235, 235,  16},  // Yellow
    { 16, 235, 235},  // Cyan
    { 16, 235,  16},  // Green
    {235,  16, 235},  // Magenta
    {235,  16,  16},  // Red
    { 16,  16, 235},  // Blue
    { 16,  16,  16},  // Black
};

void generateRGBATestPattern(TestPatternType type, 
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t>& data)
{
    data.resize(width * height * 4);
    
    switch (type) {
        case TestPatternType::ColorBars: {
            // Handle small images: ensure at least 1 pixel per bar
            uint32_t barWidth = std::max(1u, width / 8);
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t barIndex = std::min(x / barWidth, 7u);
                    uint32_t offset = (y * width + x) * 4;
                    data[offset + 0] = colorBars[barIndex][0];  // R
                    data[offset + 1] = colorBars[barIndex][1];  // G
                    data[offset + 2] = colorBars[barIndex][2];  // B
                    data[offset + 3] = 255;  // A
                }
            }
            break;
        }
        
        case TestPatternType::Gradient: {
            // Handle width == 1 case: avoid division by zero
            uint32_t widthDivisor = std::max(1u, width - 1);
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint8_t val = static_cast<uint8_t>((x * 255) / widthDivisor);
                    uint32_t offset = (y * width + x) * 4;
                    data[offset + 0] = val;  // R
                    data[offset + 1] = val;  // G
                    data[offset + 2] = val;  // B
                    data[offset + 3] = 255;  // A
                }
            }
            break;
        }
        
        case TestPatternType::Checkerboard: {
            uint32_t blockSize = 8;
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    bool isWhite = ((x / blockSize) + (y / blockSize)) % 2 == 0;
                    uint8_t val = isWhite ? 235 : 16;
                    uint32_t offset = (y * width + x) * 4;
                    data[offset + 0] = val;
                    data[offset + 1] = val;
                    data[offset + 2] = val;
                    data[offset + 3] = 255;
                }
            }
            break;
        }
        
        case TestPatternType::Ramp: {
            uint32_t totalPixels = std::max(1u, width * height);
            for (uint32_t i = 0; i < width * height; i++) {
                uint8_t val = static_cast<uint8_t>((i * 256) / totalPixels);
                uint32_t offset = i * 4;
                data[offset + 0] = val;
                data[offset + 1] = val;
                data[offset + 2] = val;
                data[offset + 3] = 255;
            }
            break;
        }
        
        case TestPatternType::Solid: {
            for (size_t i = 0; i < data.size(); i += 4) {
                data[i + 0] = 128;  // R
                data[i + 1] = 128;  // G
                data[i + 2] = 128;  // B
                data[i + 3] = 255;  // A
            }
            break;
        }
        
        case TestPatternType::Random: {
            uint32_t seed = 12345;
            for (size_t i = 0; i < data.size(); i += 4) {
                // Simple LCG random
                seed = seed * 1103515245 + 12345;
                data[i + 0] = static_cast<uint8_t>((seed >> 16) & 0xFF);
                seed = seed * 1103515245 + 12345;
                data[i + 1] = static_cast<uint8_t>((seed >> 16) & 0xFF);
                seed = seed * 1103515245 + 12345;
                data[i + 2] = static_cast<uint8_t>((seed >> 16) & 0xFF);
                data[i + 3] = 255;
            }
            break;
        }
    }
}

void generateNV12TestPattern(TestPatternType type,
                             uint32_t width, uint32_t height,
                             ColorPrimaries primaries, ColorRange range,
                             std::vector<uint8_t>& yPlane,
                             std::vector<uint8_t>& uvPlane)
{
    // First generate RGBA pattern
    std::vector<uint8_t> rgba;
    generateRGBATestPattern(type, width, height, rgba);
    
    // Convert to NV12
    convertRGBAtoNV12(rgba.data(), width, height, primaries, range, yPlane, uvPlane);
}

// =============================================================================
// Bulk Conversion Functions
// =============================================================================

void convertRGBAtoNV12(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uvPlane)
{
    uint32_t uvWidth = width / 2;
    uint32_t uvHeight = height / 2;
    
    yPlane.resize(width * height);
    uvPlane.resize(uvWidth * uvHeight * 2);  // Interleaved U,V
    
    // First pass: Calculate all Y values
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = rgba + (y * width + x) * 4;
            uint8_t yVal, cbVal, crVal;
            rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
            yPlane[y * width + x] = yVal;
        }
    }
    
    // Second pass: Calculate subsampled CbCr values (2x2 box filter)
    for (uint32_t uy = 0; uy < uvHeight; uy++) {
        for (uint32_t ux = 0; ux < uvWidth; ux++) {
            // Sample 2x2 block
            double cbSum = 0.0;
            double crSum = 0.0;
            
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    uint32_t px = ux * 2 + dx;
                    uint32_t py = uy * 2 + dy;
                    const uint8_t* pixel = rgba + (py * width + px) * 4;
                    
                    uint8_t yVal, cbVal, crVal;
                    rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
                    cbSum += cbVal;
                    crSum += crVal;
                }
            }
            
            // Average the 4 samples
            uint8_t cbAvg = static_cast<uint8_t>(std::round(cbSum / 4.0));
            uint8_t crAvg = static_cast<uint8_t>(std::round(crSum / 4.0));
            
            // Store interleaved
            uint32_t uvOffset = (uy * uvWidth + ux) * 2;
            uvPlane[uvOffset + 0] = cbAvg;  // U (Cb)
            uvPlane[uvOffset + 1] = crAvg;  // V (Cr)
        }
    }
}

void convertNV12toRGBA(const uint8_t* yPlane,
                       const uint8_t* uvPlane,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& rgba)
{
    uint32_t uvWidth = width / 2;
    rgba.resize(width * height * 4);
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t yVal = yPlane[y * width + x];
            
            // Get CbCr from subsampled position
            uint32_t ux = x / 2;
            uint32_t uy = y / 2;
            uint32_t uvOffset = (uy * uvWidth + ux) * 2;
            uint8_t cbVal = uvPlane[uvOffset + 0];
            uint8_t crVal = uvPlane[uvOffset + 1];
            
            // Convert to RGB
            uint8_t r, g, b;
            ycbcrToRgb8(yVal, cbVal, crVal, primaries, range, r, g, b);
            
            uint32_t offset = (y * width + x) * 4;
            rgba[offset + 0] = r;
            rgba[offset + 1] = g;
            rgba[offset + 2] = b;
            rgba[offset + 3] = 255;
        }
    }
}

void convertRGBAtoP010(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint16_t>& yPlane,
                       std::vector<uint16_t>& uvPlane)
{
    uint32_t uvWidth = width / 2;
    uint32_t uvHeight = height / 2;
    
    yPlane.resize(width * height);
    uvPlane.resize(uvWidth * uvHeight * 2);
    
    // Convert 8-bit input to 10-bit output
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = rgba + (y * width + x) * 4;
            
            // Scale 8-bit to 10-bit
            uint16_t r10 = (static_cast<uint16_t>(pixel[0]) * 1023) / 255;
            uint16_t g10 = (static_cast<uint16_t>(pixel[1]) * 1023) / 255;
            uint16_t b10 = (static_cast<uint16_t>(pixel[2]) * 1023) / 255;
            
            uint16_t yVal, cbVal, crVal;
            rgbToYCbCr16(r10, g10, b10, 10, primaries, range, yVal, cbVal, crVal);
            yPlane[y * width + x] = yVal;
        }
    }
    
    // Subsample chroma
    for (uint32_t uy = 0; uy < uvHeight; uy++) {
        for (uint32_t ux = 0; ux < uvWidth; ux++) {
            uint32_t cbSum = 0;
            uint32_t crSum = 0;
            
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    uint32_t px = ux * 2 + dx;
                    uint32_t py = uy * 2 + dy;
                    const uint8_t* pixel = rgba + (py * width + px) * 4;
                    
                    uint16_t r10 = (static_cast<uint16_t>(pixel[0]) * 1023) / 255;
                    uint16_t g10 = (static_cast<uint16_t>(pixel[1]) * 1023) / 255;
                    uint16_t b10 = (static_cast<uint16_t>(pixel[2]) * 1023) / 255;
                    
                    uint16_t yVal, cbVal, crVal;
                    rgbToYCbCr16(r10, g10, b10, 10, primaries, range, yVal, cbVal, crVal);
                    cbSum += cbVal;
                    crSum += crVal;
                }
            }
            
            uint32_t uvOffset = (uy * uvWidth + ux) * 2;
            uvPlane[uvOffset + 0] = static_cast<uint16_t>(cbSum / 4);
            uvPlane[uvOffset + 1] = static_cast<uint16_t>(crSum / 4);
        }
    }
}

void convertRGBAtoI420(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uPlane,
                       std::vector<uint8_t>& vPlane)
{
    uint32_t uvWidth = width / 2;
    uint32_t uvHeight = height / 2;
    
    yPlane.resize(width * height);
    uPlane.resize(uvWidth * uvHeight);
    vPlane.resize(uvWidth * uvHeight);
    
    // First pass: Y values
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = rgba + (y * width + x) * 4;
            uint8_t yVal, cbVal, crVal;
            rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
            yPlane[y * width + x] = yVal;
        }
    }
    
    // Second pass: Subsampled U and V (separate planes)
    for (uint32_t uy = 0; uy < uvHeight; uy++) {
        for (uint32_t ux = 0; ux < uvWidth; ux++) {
            double cbSum = 0.0;
            double crSum = 0.0;
            
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    uint32_t px = ux * 2 + dx;
                    uint32_t py = uy * 2 + dy;
                    const uint8_t* pixel = rgba + (py * width + px) * 4;
                    
                    uint8_t yVal, cbVal, crVal;
                    rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
                    cbSum += cbVal;
                    crSum += crVal;
                }
            }
            
            uPlane[uy * uvWidth + ux] = static_cast<uint8_t>(std::round(cbSum / 4.0));
            vPlane[uy * uvWidth + ux] = static_cast<uint8_t>(std::round(crSum / 4.0));
        }
    }
}

void convertRGBAtoNV16(const uint8_t* rgba,
                       uint32_t width, uint32_t height,
                       ColorPrimaries primaries, ColorRange range,
                       std::vector<uint8_t>& yPlane,
                       std::vector<uint8_t>& uvPlane)
{
    uint32_t uvWidth = width / 2;
    
    yPlane.resize(width * height);
    uvPlane.resize(uvWidth * height * 2);  // Half width, full height, interleaved
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = rgba + (y * width + x) * 4;
            uint8_t yVal, cbVal, crVal;
            rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
            yPlane[y * width + x] = yVal;
        }
        
        // Horizontal 2:1 subsampling for chroma
        for (uint32_t ux = 0; ux < uvWidth; ux++) {
            // Average 2 horizontal pixels
            const uint8_t* p0 = rgba + (y * width + ux * 2) * 4;
            const uint8_t* p1 = rgba + (y * width + ux * 2 + 1) * 4;
            
            uint8_t y0, cb0, cr0, y1, cb1, cr1;
            rgbToYCbCr8(p0[0], p0[1], p0[2], primaries, range, y0, cb0, cr0);
            rgbToYCbCr8(p1[0], p1[1], p1[2], primaries, range, y1, cb1, cr1);
            
            uint32_t uvOffset = (y * uvWidth + ux) * 2;
            uvPlane[uvOffset + 0] = static_cast<uint8_t>((cb0 + cb1) / 2);
            uvPlane[uvOffset + 1] = static_cast<uint8_t>((cr0 + cr1) / 2);
        }
    }
}

void convertRGBAtoYUV444(const uint8_t* rgba,
                         uint32_t width, uint32_t height,
                         ColorPrimaries primaries, ColorRange range,
                         std::vector<uint8_t>& yPlane,
                         std::vector<uint8_t>& uPlane,
                         std::vector<uint8_t>& vPlane)
{
    yPlane.resize(width * height);
    uPlane.resize(width * height);
    vPlane.resize(width * height);
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = rgba + (y * width + x) * 4;
            uint8_t yVal, cbVal, crVal;
            rgbToYCbCr8(pixel[0], pixel[1], pixel[2], primaries, range, yVal, cbVal, crVal);
            
            uint32_t offset = y * width + x;
            yPlane[offset] = yVal;
            uPlane[offset] = cbVal;
            vPlane[offset] = crVal;
        }
    }
}

// =============================================================================
// Validation Functions
// =============================================================================

double calculatePSNR(const uint8_t* data1, const uint8_t* data2, 
                     size_t size, uint32_t maxValue)
{
    if (size == 0) return 0.0;
    
    double mse = 0.0;
    for (size_t i = 0; i < size; i++) {
        double diff = static_cast<double>(data1[i]) - static_cast<double>(data2[i]);
        mse += diff * diff;
    }
    mse /= static_cast<double>(size);
    
    if (mse < 1e-10) {
        return 100.0;  // Perfect match
    }
    
    double maxVal = static_cast<double>(maxValue);
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

double calculatePSNR16(const uint16_t* data1, const uint16_t* data2, 
                       size_t size, uint32_t bitDepth)
{
    if (size == 0) return 0.0;
    
    uint32_t shiftAmount = 16 - bitDepth;
    double maxVal = static_cast<double>((1 << bitDepth) - 1);
    
    double mse = 0.0;
    for (size_t i = 0; i < size; i++) {
        // Extract actual bit values from MSB-aligned storage
        double v1 = static_cast<double>(data1[i] >> shiftAmount);
        double v2 = static_cast<double>(data2[i] >> shiftAmount);
        double diff = v1 - v2;
        mse += diff * diff;
    }
    mse /= static_cast<double>(size);
    
    if (mse < 1e-10) {
        return 100.0;
    }
    
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

ValidationResult compareNV12(const uint8_t* actualY, const uint8_t* actualUV,
                             const uint8_t* expectedY, const uint8_t* expectedUV,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance)
{
    ValidationResult result;
    uint32_t uvWidth = width / 2;
    uint32_t uvHeight = height / 2;
    size_t ySize = width * height;
    (void)ySize;  // Used in PSNR calculation
    
    // Calculate PSNR for Y plane
    result.psnrY = calculatePSNR(actualY, expectedY, ySize, 255);
    
    // Calculate PSNR for U and V channels separately
    std::vector<uint8_t> actualU(uvWidth * uvHeight);
    std::vector<uint8_t> actualV(uvWidth * uvHeight);
    std::vector<uint8_t> expectedU(uvWidth * uvHeight);
    std::vector<uint8_t> expectedV(uvWidth * uvHeight);
    
    for (size_t i = 0; i < uvWidth * uvHeight; i++) {
        actualU[i] = actualUV[i * 2];
        actualV[i] = actualUV[i * 2 + 1];
        expectedU[i] = expectedUV[i * 2];
        expectedV[i] = expectedUV[i * 2 + 1];
    }
    
    result.psnrCb = calculatePSNR(actualU.data(), expectedU.data(), actualU.size(), 255);
    result.psnrCr = calculatePSNR(actualV.data(), expectedV.data(), actualV.size(), 255);
    
    // Check for errors
    result.errorCountY = 0;
    result.errorCountCb = 0;
    result.errorCountCr = 0;
    result.maxErrorY = 0.0;
    result.maxErrorCb = 0.0;
    result.maxErrorCr = 0.0;
    
    for (size_t i = 0; i < ySize; i++) {
        int diff = std::abs(static_cast<int>(actualY[i]) - static_cast<int>(expectedY[i]));
        if (diff > static_cast<int>(tolerance)) {
            result.errorCountY++;
        }
        result.maxErrorY = std::max(result.maxErrorY, static_cast<double>(diff));
    }
    
    for (size_t i = 0; i < actualU.size(); i++) {
        int diffU = std::abs(static_cast<int>(actualU[i]) - static_cast<int>(expectedU[i]));
        int diffV = std::abs(static_cast<int>(actualV[i]) - static_cast<int>(expectedV[i]));
        if (diffU > static_cast<int>(tolerance)) {
            result.errorCountCb++;
        }
        if (diffV > static_cast<int>(tolerance)) {
            result.errorCountCr++;
        }
        result.maxErrorCb = std::max(result.maxErrorCb, static_cast<double>(diffU));
        result.maxErrorCr = std::max(result.maxErrorCr, static_cast<double>(diffV));
    }
    
    // Pass if all PSNRs are above threshold and no large errors
    bool psnrPass = (result.psnrY >= 30.0 && result.psnrCb >= 30.0 && result.psnrCr >= 30.0);
    bool errorPass = (result.maxErrorY <= tolerance * 2 && 
                      result.maxErrorCb <= tolerance * 2 && 
                      result.maxErrorCr <= tolerance * 2);
    
    result.passed = psnrPass && errorPass;
    
    if (!result.passed) {
        char msg[256];
        snprintf(msg, sizeof(msg), 
                "PSNR: Y=%.2f dB, Cb=%.2f dB, Cr=%.2f dB; Max errors: Y=%.0f, Cb=%.0f, Cr=%.0f",
                result.psnrY, result.psnrCb, result.psnrCr,
                result.maxErrorY, result.maxErrorCb, result.maxErrorCr);
        result.errorMessage = msg;
    }
    
    return result;
}

ValidationResult compareRGBA(const uint8_t* actual, const uint8_t* expected,
                             uint32_t width, uint32_t height,
                             uint32_t tolerance)
{
    ValidationResult result;
    
    // Separate channels
    std::vector<uint8_t> actualR(width * height);
    std::vector<uint8_t> actualG(width * height);
    std::vector<uint8_t> actualB(width * height);
    std::vector<uint8_t> expectedR(width * height);
    std::vector<uint8_t> expectedG(width * height);
    std::vector<uint8_t> expectedB(width * height);
    
    for (size_t i = 0; i < width * height; i++) {
        actualR[i] = actual[i * 4 + 0];
        actualG[i] = actual[i * 4 + 1];
        actualB[i] = actual[i * 4 + 2];
        expectedR[i] = expected[i * 4 + 0];
        expectedG[i] = expected[i * 4 + 1];
        expectedB[i] = expected[i * 4 + 2];
    }
    
    // Calculate PSNR per channel (use Y for R, Cb for G, Cr for B)
    result.psnrY  = calculatePSNR(actualR.data(), expectedR.data(), actualR.size(), 255);
    result.psnrCb = calculatePSNR(actualG.data(), expectedG.data(), actualG.size(), 255);
    result.psnrCr = calculatePSNR(actualB.data(), expectedB.data(), actualB.size(), 255);
    
    // Check for large errors
    result.maxErrorY = 0.0;
    result.maxErrorCb = 0.0;
    result.maxErrorCr = 0.0;
    result.errorCountY = 0;
    result.errorCountCb = 0;
    result.errorCountCr = 0;
    
    for (size_t i = 0; i < width * height; i++) {
        int diffR = std::abs(static_cast<int>(actualR[i]) - static_cast<int>(expectedR[i]));
        int diffG = std::abs(static_cast<int>(actualG[i]) - static_cast<int>(expectedG[i]));
        int diffB = std::abs(static_cast<int>(actualB[i]) - static_cast<int>(expectedB[i]));
        
        if (diffR > static_cast<int>(tolerance)) result.errorCountY++;
        if (diffG > static_cast<int>(tolerance)) result.errorCountCb++;
        if (diffB > static_cast<int>(tolerance)) result.errorCountCr++;
        
        result.maxErrorY  = std::max(result.maxErrorY,  static_cast<double>(diffR));
        result.maxErrorCb = std::max(result.maxErrorCb, static_cast<double>(diffG));
        result.maxErrorCr = std::max(result.maxErrorCr, static_cast<double>(diffB));
    }
    
    // Pass if all PSNRs are above threshold
    bool psnrPass = (result.psnrY >= 30.0 && result.psnrCb >= 30.0 && result.psnrCr >= 30.0);
    bool errorPass = (result.maxErrorY <= tolerance * 2 && 
                      result.maxErrorCb <= tolerance * 2 && 
                      result.maxErrorCr <= tolerance * 2);
    
    result.passed = psnrPass && errorPass;
    
    if (!result.passed) {
        char msg[256];
        snprintf(msg, sizeof(msg), 
                "PSNR: R=%.2f dB, G=%.2f dB, B=%.2f dB; Max errors: R=%.0f, G=%.0f, B=%.0f",
                result.psnrY, result.psnrCb, result.psnrCr,
                result.maxErrorY, result.maxErrorCb, result.maxErrorCr);
        result.errorMessage = msg;
    }
    
    return result;
}

} // namespace vkfilter_test
