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

#include "StreamInput.h"
#include "TraceClient.h"

using namespace parser_trace;

namespace {

int Usage(const char* program) {
    fprintf(stderr,
            "Usage: %s --codec h264|h265|av1|vp9 --format bytestream|ivf|obu|annexb -i <stream> [-o <trace>]\n"
            "          [--offset-alignment N] [--size-alignment N] [--pool N] [--write-ivf <file>] [--fail-decode N]\n"
            "Runs NvVideoParser over the stream with no Vulkan device and writes a canonical trace of\n"
            "every parser callback.\n"
            "  bytestream  an H.264 or H.265 Annex B byte stream, given to the parser the way the decoder\n"
            "              sample gives an elementary stream: the rest of the stream on each call, with\n"
            "              partial parsing, advancing by the bytes the parser consumed\n"
            "  ivf         one packet per IVF frame (AV1, VP9)\n"
            "  obu         the AV1 low-overhead OBU format, one packet per temporal unit\n"
            "  annexb      AV1 Annex B, one packet per temporal unit\n"
            "--write-ivf also writes the AV1 temporal units the parser is given as an IVF file.\n"
            "--fail-decode N fails the decode of picture N, counted from 0, as a decoder that cannot\n"
            "decode it does.\n",
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
    int64_t failDecode = -1;
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
        } else if (arg == "--fail-decode") {
            failDecode = strtoll(argv[++i], nullptr, 0);
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
    if ((codecInfo == nullptr) || !FormatFits(*codecInfo, format) ||
        ((ivfOutput != nullptr) && (codecInfo->operation != VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR))) {
        return Usage(argv[0]);
    }
    const bool byteStream = (strcmp(format, "bytestream") == 0);

    std::vector<uint8_t> file;
    if (!ReadFile(input, file)) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }
    std::vector<Packet> packets;
    std::string error;
    if (!ReadPackets(format, file, packets, error)) {
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
    std::unique_ptr<TraceClientIf> client = CreateTraceClient(out, codec, poolSize, failDecode);
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
