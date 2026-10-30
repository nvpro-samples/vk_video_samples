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

// The recording client of the parser trace test: a VkParserVideoDecodeClient that writes a
// canonical text line for every parser callback. vk-video-parser-trace and vk-video-parser-crafted
// share it. See ../README.md for the trace format.

#ifndef PARSER_TRACE_TRACE_CLIENT_H
#define PARSER_TRACE_TRACE_CLIENT_H

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vkvideo_parser/VulkanVideoParserIf.h"
#include "NvVideoParser/nvVulkanVideoParser.h"

namespace parser_trace {

// One trace line: a tag followed by key=value tokens. Arrays print as [a,b,c] and flag sets as
// name|name, or "-" when no flag is set.
class Line {
   public:
    explicit Line(const char* tag) : m_text(tag) {}

    Line& Num(const char* key, int64_t value) {
        Key(key);
        m_text += std::to_string(value);
        return *this;
    }

    Line& Hex(const char* key, uint64_t value) {
        char text[24];
        snprintf(text, sizeof(text), "0x%" PRIx64, value);
        Key(key);
        m_text += text;
        return *this;
    }

    Line& Str(const char* key, const char* value) {
        Key(key);
        m_text += value;
        return *this;
    }

    template <typename T>
    Line& Array(const char* key, const T* values, size_t count) {
        Key(key);
        m_text += '[';
        for (size_t i = 0; i < count; i++) {
            if (i != 0) {
                m_text += ',';
            }
            m_text += std::to_string(static_cast<int64_t>(values[i]));
        }
        m_text += ']';
        return *this;
    }

    Line& Flags(const char* key, std::initializer_list<std::pair<const char*, uint32_t>> flags) {
        Key(key);
        bool any = false;
        for (const auto& flag : flags) {
            if (flag.second != 0) {
                if (any) {
                    m_text += '|';
                }
                m_text += flag.first;
                any = true;
            }
        }
        if (!any) {
            m_text += '-';
        }
        return *this;
    }

    void Emit(FILE* out) const {
        fputs(m_text.c_str(), out);
        fputc('\n', out);
    }

   private:
    void Key(const char* key) {
        m_text += ' ';
        m_text += key;
        m_text += '=';
    }

    std::string m_text;
};

uint32_t Crc32(const uint8_t* data, size_t size);

struct CodecInfo {
    const char* name;
    VkVideoCodecOperationFlagBitsKHR operation;
    VkExtensionProperties stdExtension;
};

// The codec a --codec name selects (h264, h265, av1, vp9), or nullptr.
const CodecInfo* FindCodec(const char* name);

// The trace client. VkParserVideoDecodeClient's destructor is protected; this interface lets the
// caller own the client, which has to outlive the parser: the parser releases its pictures when it
// is destroyed.
class TraceClientIf : public VkParserVideoDecodeClient {
   public:
    ~TraceClientIf() override = default;
    virtual uint64_t Decoded() const = 0;
    virtual uint64_t Displayed() const = 0;
};

// Writes every callback to out. Pictures come from a pool of poolSize, lowest free index first; an
// allocation with none free fails and writes an alloc-failed line. With a non-negative failDecode,
// the decode of that picture, counted from 0, fails as a decoder that cannot decode it fails, and
// writes a pic-failed line.
std::unique_ptr<TraceClientIf> CreateTraceClient(FILE* out, VkVideoCodecOperationFlagBitsKHR codec, uint32_t poolSize,
                                                 int64_t failDecode = -1);

// A bitstream buffer in host memory of at least size bytes, holding initialData: what a
// GetBitstreamBuffer callback hands the parser. Returns the buffer's size.
VkDeviceSize CreateHostBitstreamBuffer(VkDeviceSize size, VkDeviceSize offsetAlignment, VkDeviceSize sizeAlignment,
                                       const uint8_t* initialData, VkDeviceSize initialSize,
                                       VkSharedBaseObj<VulkanBitstreamBuffer>& buffer);

// The host bitstream buffers of one client. Get() hands out a buffer the parser has released when one
// has the size and alignments a new buffer would have, zero-filled and holding initialData as a new
// one does, so the parser cannot tell the two apart; otherwise it creates one. A new buffer per
// picture is allocated, filled and freed each time, which under AddressSanitizer is most of a long
// stream's run time.
class HostBitstreamBufferPool {
   public:
    VkDeviceSize Get(VkDeviceSize size, VkDeviceSize offsetAlignment, VkDeviceSize sizeAlignment, const uint8_t* initialData,
                     VkDeviceSize initialSize, VkSharedBaseObj<VulkanBitstreamBuffer>& buffer);

   private:
    std::vector<VkSharedBaseObj<VulkanBitstreamBuffer>> m_buffers;
};

// One IVF frame or AV1 temporal unit: one packet for the parser.
struct Packet {
    std::vector<uint8_t> data;
    int64_t pts;
};

// Writes the packets as an IVF file with the fourcc ("AV01", "VP90").
bool WriteIvf(const char* path, const char* fourcc, const std::vector<Packet>& packets);

// Creates the parser for the codec, with the client and the bitstream buffer alignments.
VkResult CreateParser(const CodecInfo& codec, TraceClientIf& client, uint32_t offsetAlignment, uint32_t sizeAlignment,
                      VkSharedBaseObj<VulkanVideoDecodeParser>& parser);

// Gives the parser one packet per element of packets, each the end of a picture, with its PTS.
void ParsePackets(VulkanVideoDecodeParser& parser, const std::vector<Packet>& packets, FILE* out);

// Ends the stream: an end-of-stream packet, then the eos line with the client's counts.
void EndStream(VulkanVideoDecodeParser& parser, const TraceClientIf& client, FILE* out);

// Gives the parser a whole byte stream as the decoder sample does for an elementary stream
// (VulkanVideoProcessor::ParserProcessNextDataChunk): the rest of the stream on each call, with
// bPartialParsing, which returns after the first decode or display callback. The parser always
// consumes at least one byte of a non-empty packet.
void ParseByteStreamPartially(VulkanVideoDecodeParser& parser, const std::vector<uint8_t>& stream, FILE* out);

// The parser's log callback: prints to stderr.
void ParserLog(const char* format, ...);

}  // namespace parser_trace

#endif  // PARSER_TRACE_TRACE_CLIENT_H
