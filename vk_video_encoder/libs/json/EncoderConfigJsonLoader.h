/*
 * Copyright 2024-2025 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef VKVIDEOENCODER_JSON_ENCODERCONFIGJSONLOADER_H_
#define VKVIDEOENCODER_JSON_ENCODERCONFIGJSONLOADER_H_

#include "vulkan/vulkan.h"

// Load encoder config from a JSON file (see json_config/encoder_config.schema.json).
// Precedence: JSON is processed first; then CLI args override.
// Returns 0 on success, -1 on error (file open or parse).
int LoadEncoderConfigFromJson(const char* path, void* encoderConfig);

// The codec a JSON file names, VK_VIDEO_CODEC_OPERATION_NONE_KHR when it names none.
// Returns 0, or -1 when the file cannot be read or parsed, or names a codec the schema does not
// define.
int ReadEncoderCodecFromJson(const char* path, VkVideoCodecOperationFlagBitsKHR* codec);

#endif
