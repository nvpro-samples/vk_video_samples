/*
 * Copyright 2024-2025 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef VKVIDEOENCODER_JSON_ENCODERCONFIGJSONLOADER_H_
#define VKVIDEOENCODER_JSON_ENCODERCONFIGJSONLOADER_H_

// Load encoder config from a JSON file (see json_config/encoder_config.schema.json).
// Precedence: JSON is processed first; then CLI args override.
// Returns 0 on success, -1 on error (file open or parse).
int LoadEncoderConfigFromJson(const char* path, void* encoderConfig);

#endif
