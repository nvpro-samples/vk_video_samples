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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERPLANEPSNR_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERPLANEPSNR_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

// One plane of samples as memory holds them: rowPitch bytes from one row to the next, one or two
// bytes a sample (two: 16-bit little endian), and for two-byte samples the left shift that puts a
// sample's value in the high bits of its 16 bits: 0 when it is there already (a G10X6 or G12X4
// image), 16 minus the bit depth when it is in the low bits (a yuv420p10le file).
struct VkVideoEncoderPlaneSamples {
    const uint8_t* data;
    size_t rowPitch;
    uint32_t bytesPerSample;
    uint32_t msbShift;
};

// The sample at x, y of the plane, as a bitDepth-bit value. A one-byte sample of a deeper plane is
// scaled up as the encoder's input filter scales it.
inline uint32_t VkVideoEncoderSampleValue(const VkVideoEncoderPlaneSamples& plane, uint32_t x, uint32_t y, uint32_t bitDepth) {
    const uint8_t* sample = plane.data + (y * plane.rowPitch) + (static_cast<size_t>(x) * plane.bytesPerSample);
    if (plane.bytesPerSample == 1) {
        return static_cast<uint32_t>(sample[0]) << (bitDepth - 8);
    }
    const uint16_t word = static_cast<uint16_t>(sample[0] | (sample[1] << 8));
    return static_cast<uint16_t>(word << plane.msbShift) >> (16 - bitDepth);
}

// The PSNR of a width by height plane against its reconstruction, both read as bitDepth-bit values,
// with the peak (1 << bitDepth) - 1: 100 for identical planes, -1 for an empty one.
inline double VkVideoEncoderPlanePsnr(const VkVideoEncoderPlaneSamples& source, const VkVideoEncoderPlaneSamples& recon,
                                      uint32_t width, uint32_t height, uint32_t bitDepth) {
    const uint64_t samples = static_cast<uint64_t>(width) * height;
    if ((samples == 0) || (source.data == nullptr) || (recon.data == nullptr)) {
        return -1.0;
    }
    uint64_t sumSqDiff = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const int64_t d = static_cast<int64_t>(VkVideoEncoderSampleValue(source, x, y, bitDepth)) -
                              static_cast<int64_t>(VkVideoEncoderSampleValue(recon, x, y, bitDepth));
            sumSqDiff += static_cast<uint64_t>(d * d);
        }
    }
    const double mse = static_cast<double>(sumSqDiff) / static_cast<double>(samples);
    const double peak = static_cast<double>((1u << bitDepth) - 1);
    return (mse <= 1e-10) ? 100.0 : (10.0 * log10((peak * peak) / mse));
}

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERPLANEPSNR_H_ */
