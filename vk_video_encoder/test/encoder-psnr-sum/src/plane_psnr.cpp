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

/*
 * VkVideoEncoderPlanePsnr, without a device.
 *
 * The encoder's --psnr compares a captured input plane with the reconstruction read back from the
 * DPB. Above 8 bits the two store a sample differently: the input as the file has it (a
 * yuv420p10le file in the low bits of 16, a P010 file in the high bits), the reconstruction in the
 * high bits (G10X6, G12X4). Each must be read as the same bit-depth value, and the peak is the bit
 * depth's.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "VkVideoEncoder/VkVideoEncoderPlanePsnr.h"

namespace {

int g_failures = 0;

void CheckPsnr(const char* name, double psnr, double expected) {
    if (std::fabs(psnr - expected) > 1e-9) {
        std::fprintf(stderr, "FAIL %s: PSNR %.12f, expected %.12f\n", name, psnr, expected);
        g_failures++;
    }
}

constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 8;

// A plane of 16-bit little-endian words, each sample's value shifted left by shift.
std::vector<uint8_t> Words(const std::vector<uint32_t>& values, uint32_t shift) {
    std::vector<uint8_t> bytes;
    for (uint32_t value : values) {
        const uint16_t word = static_cast<uint16_t>(value << shift);
        bytes.push_back(static_cast<uint8_t>(word & 0xff));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
    }
    return bytes;
}

// Bit-depth values that cover the range: a ramp, with the extremes.
std::vector<uint32_t> Ramp(uint32_t bitDepth) {
    const uint32_t peak = (1u << bitDepth) - 1;
    std::vector<uint32_t> values(kWidth * kHeight);
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = static_cast<uint32_t>((i * peak) / (values.size() - 1));
    }
    return values;
}

// The PSNR of a plane in which one sample in every two differs by one: mse 0.5.
double HalfOffByOne(uint32_t bitDepth) {
    const double peak = static_cast<double>((1u << bitDepth) - 1);
    return 10.0 * std::log10((peak * peak) / 0.5);
}

std::vector<uint32_t> OddSamplesOffByOne(std::vector<uint32_t> values) {
    for (size_t i = 1; i < values.size(); i += 2) {
        values[i] = (values[i] == 0) ? 1 : (values[i] - 1);
    }
    return values;
}

void EightBit() {
    std::vector<uint8_t> source(kWidth * kHeight), recon(kWidth * kHeight);
    for (size_t i = 0; i < source.size(); i++) {
        source[i] = static_cast<uint8_t>(i * 2);
        recon[i] = static_cast<uint8_t>(source[i] + ((i % 2) ? 1 : 0));
    }
    const VkVideoEncoderPlaneSamples s{source.data(), kWidth, 1, 0};
    const VkVideoEncoderPlaneSamples r{recon.data(), kWidth, 1, 0};
    CheckPsnr("8-bit identical", VkVideoEncoderPlanePsnr(s, s, kWidth, kHeight, 8), 100.0);
    CheckPsnr("8-bit off by one", VkVideoEncoderPlanePsnr(s, r, kWidth, kHeight, 8), HalfOffByOne(8));
}

// A file's samples in the low bits (msbShift 16 - bitDepth) or the high bits (0) against a
// reconstruction in the high bits.
void Deep(uint32_t bitDepth) {
    const std::vector<uint32_t> values = Ramp(bitDepth);
    const std::vector<uint8_t> lowBits = Words(values, 0);
    const std::vector<uint8_t> highBits = Words(values, 16 - bitDepth);
    const std::vector<uint8_t> reconOff = Words(OddSamplesOffByOne(values), 16 - bitDepth);
    const VkVideoEncoderPlaneSamples fileLow{lowBits.data(), kWidth * 2, 2, 16 - bitDepth};
    const VkVideoEncoderPlaneSamples fileHigh{highBits.data(), kWidth * 2, 2, 0};
    const VkVideoEncoderPlaneSamples recon{highBits.data(), kWidth * 2, 2, 0};
    const VkVideoEncoderPlaneSamples off{reconOff.data(), kWidth * 2, 2, 0};
    const std::string depth = std::to_string(bitDepth) + "-bit ";
    CheckPsnr((depth + "low-bit file, identical").c_str(), VkVideoEncoderPlanePsnr(fileLow, recon, kWidth, kHeight, bitDepth),
              100.0);
    CheckPsnr((depth + "high-bit file, identical").c_str(), VkVideoEncoderPlanePsnr(fileHigh, recon, kWidth, kHeight, bitDepth),
              100.0);
    CheckPsnr((depth + "low-bit file, off by one").c_str(), VkVideoEncoderPlanePsnr(fileLow, off, kWidth, kHeight, bitDepth),
              HalfOffByOne(bitDepth));
    // The bytes alone are not the values: read as 8-bit samples, a low-bit file does not match.
    const VkVideoEncoderPlaneSamples fileLowAsBytes{lowBits.data(), kWidth * 2, 1, 0};
    const VkVideoEncoderPlaneSamples reconAsBytes{highBits.data(), kWidth * 2, 1, 0};
    if (VkVideoEncoderPlanePsnr(fileLowAsBytes, reconAsBytes, kWidth * 2, kHeight, 8) >= 100.0) {
        std::fprintf(stderr, "FAIL %s: the bytes of the two layouts match\n", depth.c_str());
        g_failures++;
    }
}

// An 8-bit source of a 10-bit encode: each sample scaled as the input filter scales it.
void EightBitSourceOfDeeperEncode() {
    std::vector<uint8_t> source(kWidth * kHeight);
    std::vector<uint32_t> scaled(kWidth * kHeight);
    for (size_t i = 0; i < source.size(); i++) {
        source[i] = static_cast<uint8_t>(i * 2);
        scaled[i] = static_cast<uint32_t>(source[i]) << 2;
    }
    const std::vector<uint8_t> reconBytes = Words(scaled, 6);
    const VkVideoEncoderPlaneSamples s{source.data(), kWidth, 1, 0};
    const VkVideoEncoderPlaneSamples r{reconBytes.data(), kWidth * 2, 2, 0};
    CheckPsnr("8-bit source, 10-bit reconstruction", VkVideoEncoderPlanePsnr(s, r, kWidth, kHeight, 10), 100.0);
}

void EmptyPlane() {
    const uint8_t sample = 0;
    const VkVideoEncoderPlaneSamples s{&sample, 1, 1, 0};
    const VkVideoEncoderPlaneSamples none{nullptr, 1, 1, 0};
    CheckPsnr("no samples", VkVideoEncoderPlanePsnr(s, s, 0, 0, 8), -1.0);
    CheckPsnr("no reconstruction", VkVideoEncoderPlanePsnr(s, none, 1, 1, 8), -1.0);
}

}  // namespace

int main() {
    EightBit();
    Deep(10);
    Deep(12);
    EightBitSourceOfDeeperEncode();
    EmptyPlane();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS: the plane PSNR reads each layout's samples as bit-depth values\n");
    return 0;
}
