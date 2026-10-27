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

// vk-video-parser-trace runs NvVideoParser over an H.264, H.265, AV1 or VP9 stream with no Vulkan
// device and writes a canonical text trace of every callback the parser makes: sequence info,
// parameter sets, each picture with its DPB snapshot and a CRC32 of its bytes, and the display
// order.
//
// The trace is deterministic for a given parser and stream, so a golden comparison catches any
// change in what the parser hands a decoder. scripts/av1_reference_model.py checks the AV1 fields
// against the AV1 specification. See ../README.md.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "TraceClient.h"

using namespace parser_trace;

namespace {

bool ReadFile(const char* path, std::vector<uint8_t>& data) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t chunk[1 << 16];
    size_t count = 0;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        data.insert(data.end(), chunk, chunk + count);
    }
    const bool ok = (ferror(file) == 0);
    fclose(file);
    return ok;
}

uint32_t ReadLe32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// AV1 leb128() (AV1 4.10.5). Returns the number of bytes read, or 0 when the value is truncated.
size_t ReadLeb128(const std::vector<uint8_t>& data, size_t pos, uint64_t& value) {
    value = 0;
    for (size_t i = 0; i < 8; i++) {
        if (pos + i >= data.size()) {
            return 0;
        }
        value |= static_cast<uint64_t>(data[pos + i] & 0x7f) << (7 * i);
        if ((data[pos + i] & 0x80) == 0) {
            return i + 1;
        }
    }
    return 0;
}

void WriteLeb128(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        out.push_back(byte | ((value != 0) ? 0x80 : 0));
    } while (value != 0);
}

bool ReadIvf(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    if ((file.size() < 32) || (memcmp(file.data(), "DKIF", 4) != 0)) {
        error = "not an IVF file";
        return false;
    }
    size_t pos = file[6] | (file[7] << 8);
    while (pos + 12 <= file.size()) {
        const uint32_t size = ReadLe32(&file[pos]);
        const int64_t pts =
            static_cast<int64_t>(ReadLe32(&file[pos + 4]) | (static_cast<uint64_t>(ReadLe32(&file[pos + 8])) << 32));
        pos += 12;
        if (size > file.size() - pos) {
            error = "truncated IVF frame";
            return false;
        }
        packets.push_back({std::vector<uint8_t>(file.begin() + pos, file.begin() + pos + size), pts});
        pos += size;
    }
    return true;
}

// Low-overhead OBU stream (AV1 5.2): every OBU carries its size. A temporal delimiter starts a
// temporal unit, which is one parser packet.
bool ReadObuStream(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    std::vector<uint8_t> unit;
    size_t pos = 0;
    while (pos < file.size()) {
        const uint8_t header = file[pos];
        const uint32_t type = (header >> 3) & 0xf;
        const size_t headerSize = 1 + ((header >> 2) & 1);
        if ((header & 0x02) == 0) {
            error = "OBU without obu_has_size_field in a low-overhead stream";
            return false;
        }
        uint64_t payloadSize = 0;
        const size_t lebSize = ReadLeb128(file, pos + headerSize, payloadSize);
        if ((lebSize == 0) || (payloadSize > file.size() - (pos + headerSize + lebSize))) {
            error = "truncated OBU";
            return false;
        }
        const size_t obuSize = headerSize + lebSize + payloadSize;
        if ((type == 2) && !unit.empty()) {  // OBU_TEMPORAL_DELIMITER
            packets.push_back({unit, static_cast<int64_t>(packets.size())});
            unit.clear();
        }
        unit.insert(unit.end(), file.begin() + pos, file.begin() + pos + obuSize);
        pos += obuSize;
    }
    if (!unit.empty()) {
        packets.push_back({unit, static_cast<int64_t>(packets.size())});
    }
    return true;
}

// Annex B stream (AV1 B.2): temporal_unit(size) of frame_unit(size) of obu_length + OBU. Each OBU
// is rewritten with obu_has_size_field set, so every temporal unit becomes one low-overhead packet.
bool ReadAnnexB(const std::vector<uint8_t>& file, std::vector<Packet>& packets, std::string& error) {
    size_t pos = 0;
    while (pos < file.size()) {
        uint64_t unitSize = 0;
        size_t lebSize = ReadLeb128(file, pos, unitSize);
        if ((lebSize == 0) || (unitSize > file.size() - (pos + lebSize))) {
            error = "truncated temporal unit";
            return false;
        }
        pos += lebSize;
        const size_t unitEnd = pos + unitSize;
        std::vector<uint8_t> unit;
        while (pos < unitEnd) {
            uint64_t frameUnitSize = 0;
            lebSize = ReadLeb128(file, pos, frameUnitSize);
            if ((lebSize == 0) || (frameUnitSize > unitEnd - (pos + lebSize))) {
                error = "truncated frame unit";
                return false;
            }
            pos += lebSize;
            const size_t frameUnitEnd = pos + frameUnitSize;
            while (pos < frameUnitEnd) {
                uint64_t obuLength = 0;
                lebSize = ReadLeb128(file, pos, obuLength);
                if ((lebSize == 0) || (obuLength == 0) || (obuLength > frameUnitEnd - (pos + lebSize))) {
                    error = "truncated OBU";
                    return false;
                }
                pos += lebSize;
                const uint8_t header = file[pos];
                const size_t headerSize = 1 + ((header >> 2) & 1);
                if (obuLength < headerSize) {
                    error = "OBU shorter than its header";
                    return false;
                }
                if ((header & 0x02) != 0) {
                    unit.insert(unit.end(), file.begin() + pos, file.begin() + pos + obuLength);
                } else {
                    unit.push_back(header | 0x02);
                    unit.insert(unit.end(), file.begin() + pos + 1, file.begin() + pos + headerSize);
                    WriteLeb128(unit, obuLength - headerSize);
                    unit.insert(unit.end(), file.begin() + pos + headerSize, file.begin() + pos + obuLength);
                }
                pos += obuLength;
            }
        }
        packets.push_back({unit, static_cast<int64_t>(packets.size())});
    }
    return true;
}

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec h264|h265|av1|vp9 --format bytestream|ivf|obu|annexb -i <stream> [-o <trace>]\n"
            "          [--offset-alignment N] [--size-alignment N] [--pool N] [--write-ivf <file>]\n"
            "Runs NvVideoParser over the stream with no Vulkan device and writes a canonical trace of\n"
            "every parser callback.\n"
            "  bytestream  an H.264 or H.265 Annex B byte stream, given to the parser the way the decoder\n"
            "              sample gives an elementary stream: the rest of the stream on each call, with\n"
            "              partial parsing, advancing by the bytes the parser consumed\n"
            "  ivf         one packet per IVF frame (AV1, VP9)\n"
            "  obu         the AV1 low-overhead OBU format, one packet per temporal unit\n"
            "  annexb      AV1 Annex B, one packet per temporal unit\n"
            "--write-ivf also writes the AV1 temporal units the parser is given as an IVF file.\n",
            program);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* codecName = nullptr;
    const char* format = nullptr;
    const char* input = nullptr;
    const char* output = nullptr;
    const char* ivfOutput = nullptr;
    uint32_t offsetAlignment = 256;
    uint32_t sizeAlignment = 256;
    uint32_t poolSize = 32;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return Usage(argv[0]);
        }
        if (arg == "--codec") {
            codecName = argv[++i];
        } else if (arg == "--format") {
            format = argv[++i];
        } else if (arg == "-i") {
            input = argv[++i];
        } else if (arg == "-o") {
            output = argv[++i];
        } else if (arg == "--offset-alignment") {
            offsetAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--size-alignment") {
            sizeAlignment = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--pool") {
            poolSize = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--write-ivf") {
            ivfOutput = argv[++i];
        } else {
            return Usage(argv[0]);
        }
    }
    if ((codecName == nullptr) || (format == nullptr) || (input == nullptr)) {
        return Usage(argv[0]);
    }
    const CodecInfo* codecInfo = FindCodec(codecName);
    const bool isAv1 = (codecInfo != nullptr) && (codecInfo->operation == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);
    const bool isH26x = (codecInfo != nullptr) && ((codecInfo->operation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) ||
                                                   (codecInfo->operation == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR));
    const bool byteStream = (strcmp(format, "bytestream") == 0);
    bool formatFits = false;
    if (byteStream) {
        formatFits = isH26x;
    } else if (strcmp(format, "ivf") == 0) {
        formatFits = !isH26x;
    } else if ((strcmp(format, "obu") == 0) || (strcmp(format, "annexb") == 0)) {
        formatFits = isAv1;
    }
    if ((codecInfo == nullptr) || !formatFits || ((ivfOutput != nullptr) && !isAv1)) {
        return Usage(argv[0]);
    }

    std::vector<uint8_t> file;
    if (!ReadFile(input, file)) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }
    std::vector<Packet> packets;
    std::string error;
    bool read = true;
    if (strcmp(format, "ivf") == 0) {
        read = ReadIvf(file, packets, error);
    } else if (strcmp(format, "obu") == 0) {
        read = ReadObuStream(file, packets, error);
    } else if (strcmp(format, "annexb") == 0) {
        read = ReadAnnexB(file, packets, error);
    }
    if (!read) {
        fprintf(stderr, "%s: %s\n", input, error.c_str());
        return 1;
    }
    if ((ivfOutput != nullptr) && !WriteIvf(ivfOutput, "AV01", packets)) {
        fprintf(stderr, "Cannot write %s\n", ivfOutput);
        return 1;
    }

    FILE* out = stdout;
    if (output != nullptr) {
        out = fopen(output, "w");
        if (out == nullptr) {
            fprintf(stderr, "Cannot write %s\n", output);
            return 1;
        }
    }

    Line trace("trace");
    trace.Num("version", 1).Str("codec", codecName).Str("format", format);
    if (byteStream) {
        trace.Num("bytes", static_cast<int64_t>(file.size()));
    } else {
        trace.Num("packets", static_cast<int64_t>(packets.size()));
    }
    trace.Num("offsetAlignment", offsetAlignment).Num("sizeAlignment", sizeAlignment).Num("pool", poolSize).Emit(out);

    const VkVideoCodecOperationFlagBitsKHR codec = codecInfo->operation;
    // The client outlives the parser: the parser releases its pictures when it is destroyed.
    std::unique_ptr<TraceClientIf> client = CreateTraceClient(out, codec, poolSize);
    int status = 0;
    {
        VkSharedBaseObj<VulkanVideoDecodeParser> parser;
        const VkResult result = CreateParser(*codecInfo, *client, offsetAlignment, sizeAlignment, parser);
        if ((result != VK_SUCCESS) || !parser) {
            fprintf(stderr, "CreateVulkanVideoDecodeParser failed: %d\n", static_cast<int>(result));
            status = 1;
        } else {
            if (byteStream) {
                ParseByteStreamPartially(*parser, file, out);
            }
            ParsePackets(*parser, packets, out);
            EndStream(*parser, *client, out);
        }
    }

    if (out != stdout) {
        fclose(out);
    }
    return status;
}
