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

// The parser fuzz targets. fuzz_parser_h264, fuzz_parser_h265, fuzz_parser_av1 and
// fuzz_parser_vp9 give NvVideoParser an input the fuzzer chooses, through the recording client of
// the trace test, which reads everything the parser hands a client. They are built with libFuzzer
// (BUILD_FUZZERS, clang). Without libFuzzer the same source builds vk-video-parser-fuzz-replay,
// which runs inputs from files, or the seed inputs it derives from a stream, so CTest replays them
// with any compiler and any sanitizer.
//
// An input is a mode byte, a 32-bit little-endian seed, and the payload:
//   mode & 3 == 0  the payload is a byte stream, given as the decoder sample gives an elementary
//                  stream: the rest of it on each call, with partial parsing;
//   mode & 3 == 1  the payload is cut into chunks of 1 to 4096 bytes, each a call; the seed draws
//                  the sizes;
//   mode & 3 == 2, 3  the payload is a list of packets, each a 32-bit little-endian size and its
//                  bytes, as IVF frames or AV1 temporal units; a size past the end takes the rest;
//   mode & 4       each packet ends a picture (bEOP);
//   mode & 8       each packet carries a timestamp.
// The stream then ends with an end-of-stream packet.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "StreamInput.h"
#include "TraceClient.h"

using namespace parser_trace;

namespace {

const CodecInfo* g_codec = nullptr;

FILE* NullOutput() {
#ifdef _WIN32
    static FILE* out = fopen("NUL", "w");
#else
    static FILE* out = fopen("/dev/null", "w");
#endif
    return out;
}

uint32_t ReadLe32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// xorshift32: the chunk sizes of mode 1.
uint32_t NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void Parse(VulkanVideoDecodeParser& parser, const uint8_t* data, size_t size, uint8_t mode, int64_t& pts) {
    VkParserBitstreamPacket packet = {};
    packet.pByteStream = data;
    packet.nDataLength = size;
    packet.bEOP = (mode & 4) ? 1 : 0;
    packet.bPTSValid = (mode & 8) ? 1 : 0;
    packet.llPTS = pts++;
    size_t parsedBytes = 0;
    parser.ParseByteStream(&packet, &parsedBytes);
}

void RunInput(const CodecInfo& codec, const uint8_t* data, size_t size) {
    if (size < 5) {
        return;
    }
    const uint8_t mode = data[0];
    uint32_t seed = ReadLe32(data + 1);
    if (seed == 0) {
        seed = 1;
    }
    const uint8_t* payload = data + 5;
    const size_t length = size - 5;

    // The client outlives the parser: the parser releases its pictures when it is destroyed.
    std::unique_ptr<TraceClientIf> client = CreateTraceClient(NullOutput(), codec.operation, 32);
    VkSharedBaseObj<VulkanVideoDecodeParser> parser;
    if ((CreateParser(codec, *client, 256, 256, parser) != VK_SUCCESS) || !parser) {
        return;
    }
    int64_t pts = 0;
    switch (mode & 3) {
        case 0: {
            const std::vector<uint8_t> stream(payload, payload + length);
            ParseByteStreamPartially(*parser, stream, NullOutput());
            break;
        }
        case 1:
            for (size_t offset = 0; offset < length;) {
                const size_t chunk = std::min<size_t>(length - offset, 1 + NextRandom(seed) % 4096);
                Parse(*parser, payload + offset, chunk, mode, pts);
                offset += chunk;
            }
            break;
        default:
            for (size_t offset = 0; offset < length;) {
                size_t packetSize = length - offset;
                if (packetSize >= 4) {
                    packetSize = std::min<size_t>(ReadLe32(payload + offset), packetSize - 4);
                    offset += 4;
                } else {
                    offset = length - packetSize;
                }
                Parse(*parser, payload + offset, packetSize, mode, pts);
                offset += packetSize;
            }
            break;
    }
    EndStream(*parser, *client, NullOutput());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    RunInput(*g_codec, data, size);
    return 0;
}

#ifdef PARSER_FUZZ_CODEC

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    g_codec = FindCodec(PARSER_FUZZ_CODEC);
    return 0;
}

#else  // The replay tool.

#include <filesystem>

namespace {

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec h264|h265|av1|vp9 [--format bytestream|ivf|obu|annexb -i <stream>... [--write-seeds <dir>]]\n"
            "          [<input or directory>...]\n"
            "Runs the parser fuzz target over fuzz inputs, files or every file of a directory, and over\n"
            "the seed inputs derived from each stream: each packet mode with three seeds, with and without\n"
            "the end-of-picture flag. --write-seeds writes the seed inputs, a fuzzer's starting corpus.\n",
            program);
    return 2;
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

std::vector<uint8_t> Header(uint8_t mode, uint32_t seed) {
    std::vector<uint8_t> header = {mode};
    AppendLe32(header, seed);
    return header;
}

using Input = std::vector<uint8_t>;

// The seed inputs of a stream: a byte stream whole and in chunks, and the packets of a container.
std::vector<Input> SeedInputs(const char* format, const std::vector<uint8_t>& file) {
    std::vector<Input> inputs;
    std::vector<Packet> packets;
    std::string error;
    if (strcmp(format, "bytestream") == 0) {
        for (uint8_t eop : {0, 4}) {
            std::vector<uint8_t> input = Header(eop, 1);
            input.insert(input.end(), file.begin(), file.end());
            inputs.push_back(input);
            for (uint32_t seed : {1u, 2u, 3u}) {
                input = Header(static_cast<uint8_t>(1 | eop), seed);
                input.insert(input.end(), file.begin(), file.end());
                inputs.push_back(input);
            }
        }
    } else if (ReadPackets(format, file, packets, error)) {
        for (uint8_t flags : {0, 4, 12}) {
            std::vector<uint8_t> input = Header(static_cast<uint8_t>(2 | flags), 1);
            for (const Packet& packet : packets) {
                AppendLe32(input, static_cast<uint32_t>(packet.data.size()));
                input.insert(input.end(), packet.data.begin(), packet.data.end());
            }
            inputs.push_back(input);
        }
    }
    return inputs;
}

// A file, or every file of a directory in name order.
bool CollectFiles(const std::string& path, std::vector<std::string>& files) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
        files.push_back(path);
        return true;
    }
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        if (entry.is_regular_file() && (entry.path().filename().string()[0] != '.')) {
            names.push_back(entry.path().string());
        }
    }
    if (error) {
        fprintf(stderr, "Cannot read %s\n", path.c_str());
        return false;
    }
    std::sort(names.begin(), names.end());
    files.insert(files.end(), names.begin(), names.end());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* codecName = nullptr;
    const char* format = nullptr;
    std::vector<const char*> streams;
    const char* seedDir = nullptr;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if ((arg == "--codec") || (arg == "--format") || (arg == "-i") || (arg == "--write-seeds")) {
            if (i + 1 >= argc) {
                return Usage(argv[0]);
            }
            const char* value = argv[++i];
            if (arg == "--codec") {
                codecName = value;
            } else if (arg == "--format") {
                format = value;
            } else if (arg == "-i") {
                streams.push_back(value);
            } else {
                seedDir = value;
            }
        } else if (!arg.empty() && (arg[0] == '-')) {
            return Usage(argv[0]);
        } else if (!CollectFiles(arg, files)) {
            return 1;
        }
    }
    g_codec = (codecName != nullptr) ? FindCodec(codecName) : nullptr;
    if ((g_codec == nullptr) || (streams.empty() != (format == nullptr)) ||
        ((format != nullptr) && !FormatFits(*g_codec, format))) {
        return Usage(argv[0]);
    }

    if (seedDir != nullptr) {
        std::error_code error;
        std::filesystem::create_directories(seedDir, error);
        if (error) {
            fprintf(stderr, "Cannot create %s\n", seedDir);
            return 1;
        }
    }
    size_t runs = 0;
    for (const char* stream : streams) {
        std::vector<uint8_t> file;
        if (!ReadFile(stream, file)) {
            fprintf(stderr, "Cannot read %s\n", stream);
            return 1;
        }
        const std::vector<Input> inputs = SeedInputs(format, file);
        for (size_t i = 0; i < inputs.size(); i++) {
            if (seedDir != nullptr) {
                const std::string path =
                    (std::filesystem::path(seedDir) / std::filesystem::path(stream).filename()).string() + "." + std::to_string(i);
                FILE* out = fopen(path.c_str(), "wb");
                if ((out == nullptr) || (fwrite(inputs[i].data(), 1, inputs[i].size(), out) != inputs[i].size())) {
                    fprintf(stderr, "Cannot write %s\n", path.c_str());
                    return 1;
                }
                fclose(out);
            }
            LLVMFuzzerTestOneInput(inputs[i].data(), inputs[i].size());
            runs++;
        }
    }
    for (const std::string& path : files) {
        std::vector<uint8_t> input;
        if (!ReadFile(path.c_str(), input)) {
            fprintf(stderr, "Cannot read %s\n", path.c_str());
            return 1;
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
        runs++;
    }
    printf("%zu inputs run\n", runs);
    return 0;
}

#endif  // PARSER_FUZZ_CODEC
