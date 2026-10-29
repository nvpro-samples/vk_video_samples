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

// The stream formats vk-video-parser-trace and vk-video-adapter-trace read: an H.264 or H.265
// byte stream, IVF, and the AV1 low-overhead OBU and Annex B formats.

#ifndef PARSER_TRACE_STREAM_INPUT_H
#define PARSER_TRACE_STREAM_INPUT_H

#include <cstdint>
#include <string>
#include <vector>

#include "TraceClient.h"

namespace parser_trace {

bool ReadFile(const char* path, std::vector<uint8_t>& data);

// Whether --format suits the codec: bytestream for H.264 and H.265, ivf for AV1 and VP9, obu and
// annexb for AV1.
bool FormatFits(const CodecInfo& codec, const char* format);

// Splits the file into the packets the parser is given: one per IVF frame, or one per AV1
// temporal unit. A byte stream has no packets: it is given whole (ParseByteStreamPartially).
bool ReadPackets(const char* format, const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error);

}  // namespace parser_trace

#endif  // PARSER_TRACE_STREAM_INPUT_H
