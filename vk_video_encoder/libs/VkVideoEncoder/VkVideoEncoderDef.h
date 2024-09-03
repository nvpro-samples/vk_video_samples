/*
 * Copyright 2023 NVIDIA Corporation.
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

#ifndef _VKVIDEOENCODERDEF_H_
#define _VKVIDEOENCODERDEF_H_

#include <limits.h>
#include "vulkan/vulkan.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h264std_encode.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_h265std_encode.h"

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) ((sizeof(a) / sizeof(a[0])))
#endif
#endif

static const uint32_t H264MbSizeAlignment = 16;

template<typename sizeType>
sizeType AlignSize(sizeType size, sizeType alignment) {
    assert((alignment & (alignment - 1)) == 0);
    return (size + alignment -1) & ~(alignment -1);
}

template<typename valuesType>
valuesType DivUp(valuesType value, valuesType divisor) {
    assert(divisor != (valuesType)0);
    return (value + (divisor - 1)) / divisor;
}

template<typename valueType>
uint32_t FastIntLog2(valueType val)
{
    uint32_t log2 = 0;
    while (val != 0) {
        val >>= 1;
        log2++;
    }

    return log2;
}

template<typename valueType>
static inline valueType IntAbs(valueType x)
{
    static const int inBits = (sizeof(valueType) * CHAR_BIT) - 1;
    valueType y = x >> inBits;
    return (x ^ y) - y;
}

// greatest common divisor
template<typename valuesType>
static valuesType Gcd(valuesType u, valuesType v)
{
    if (u <= 1 || v <= 1) {
        return 1;
    }

    while (u != 0) {

        if (u >= v) {
            u -= v;
        } else {
            v -= u;
        }
    }
    return v;
}

/**
 * QP value for frames
 */
struct ConstQpSettings
{
    ConstQpSettings()
        : qpInterP(0)
        , qpInterB(0)
        , qpIntra(0)
    { }

    uint32_t qpInterP;
    uint32_t qpInterB;
    uint32_t qpIntra;
};

#endif /* _VKVIDEOENCODERDEF_H_ */
