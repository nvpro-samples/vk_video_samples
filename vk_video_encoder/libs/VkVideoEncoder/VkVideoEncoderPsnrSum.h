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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERPSNRSUM_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERPSNRSUM_H_

#include <cmath>
#include <cstdint>

// The sum of the frames' PSNR values, exact, so that it does not depend on the order the frames are
// added in: the assembly threads finish frames in the order their fences complete, which varies from
// run to run, and a floating-point sum depends on the order of its terms. Each value is rounded to a
// multiple of 2^-32 dB and summed in 128 bits, which no number of frames can overflow.
class VkVideoEncoderPsnrSum {
   public:
    // A frame's PSNR is between 0 and 100 dB; a negative or NaN value is not added.
    void Add(double psnr) {
        if (!(psnr >= 0.0)) {
            return;
        }
        const uint64_t units = static_cast<uint64_t>(std::llround(psnr * kUnitsPerDb));
        const uint64_t low = m_low + units;
        m_high += (low < m_low) ? 1 : 0;
        m_low = low;
    }

    double Value() const { return (std::ldexp(static_cast<double>(m_high), 64) + static_cast<double>(m_low)) / kUnitsPerDb; }

    void Reset() {
        m_low = 0;
        m_high = 0;
    }

   private:
    static constexpr double kUnitsPerDb = 4294967296.0;  // 2^32
    uint64_t m_low = 0;
    uint64_t m_high = 0;
};

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERPSNRSUM_H_ */
