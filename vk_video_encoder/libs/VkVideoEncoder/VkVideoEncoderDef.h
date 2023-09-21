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

#include "vulkan/vulkan.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h264std_encode.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_h265std_encode.h"

#define MAX_REFS 16
#define MAX_DPB_SIZE 16
#define MAX_MMCOS 16        // max mmco commands.
#define MAX_REFPIC_CMDS 16  // max reorder commands.

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) ((sizeof(a) / sizeof(a[0])))
#endif

static const uint32_t H264MbSizeAlignment = 16;

template<typename sizeType>
sizeType AlignSize(sizeType size, sizeType alignment) {
    assert((alignment & (alignment - 1)) == 0);
    return (size + alignment -1) & ~(alignment -1);
}

typedef enum StdVideoH26XPictureType {
    STD_VIDEO_H26X_PICTURE_TYPE_P = STD_VIDEO_H265_PICTURE_TYPE_P,
    STD_VIDEO_H26X_PICTURE_TYPE_B = STD_VIDEO_H265_PICTURE_TYPE_B,
    STD_VIDEO_H26X_PICTURE_TYPE_I = STD_VIDEO_H265_PICTURE_TYPE_I,
    STD_VIDEO_H26X_PICTURE_TYPE_IDR = STD_VIDEO_H265_PICTURE_TYPE_IDR,
    STD_VIDEO_H26X_PICTURE_TYPE_INTRA_REFRESH = 6, // Special IDR : First picture in intra refresh cycle
    STD_VIDEO_H26X_PICTURE_TYPE_INVALID = STD_VIDEO_H265_PICTURE_TYPE_INVALID,
    STD_VIDEO_H26X_PICTURE_TYPE_MAX_ENUM = STD_VIDEO_H265_PICTURE_TYPE_MAX_ENUM
} StdVideoH26XPictureType;

static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_P == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_P, "STD_VIDEO_H265_PICTURE_TYPE_P");
static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_B == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_B, "STD_VIDEO_H265_PICTURE_TYPE_B");
static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_I == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_I, "STD_VIDEO_H265_PICTURE_TYPE_I");
// static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_IDR == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_IDR, "STD_VIDEO_H265_PICTURE_TYPE_IDR");
static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_INVALID == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_INVALID, "STD_VIDEO_H265_PICTURE_TYPE_INVALID");
static_assert((uint32_t)STD_VIDEO_H264_PICTURE_TYPE_MAX_ENUM == (uint32_t)STD_VIDEO_H265_PICTURE_TYPE_MAX_ENUM, "STD_VIDEO_H265_PICTURE_TYPE_MAX_ENUM");

#endif /* _VKVIDEOENCODERDEF_H_ */
