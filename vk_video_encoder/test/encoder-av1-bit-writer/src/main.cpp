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

// The AV1 encoder's bit writer against the AV1 specification: f(n) writes n bits, the most
// significant first (4.10.2), whatever n is; leb128() writes seven bits per byte, the low group
// first, with bit 7 set on every byte but the last (4.10.5); trailing_bits() ends with a one and
// zeros to the byte boundary (5.3.4).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "VkVideoEncoder/VkVideoEncoderAV1.h"

using BitWriter = VkVideoEncoderAV1::VkVideoEncoderAV1BitWriter;

namespace {

int g_failures = 0;

std::string Hex(const std::vector<uint8_t>& bytes) {
    std::string text;
    char byte[4];
    for (uint8_t value : bytes) {
        snprintf(byte, sizeof(byte), "%s%02x", text.empty() ? "" : " ", value);
        text += byte;
    }
    return text;
}

void Expect(const char* what, const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected) {
    if (actual == expected) {
        printf("PASS %s: %s\n", what, Hex(actual).c_str());
        return;
    }
    printf("FAIL %s: %s, expected %s\n", what, Hex(actual).c_str(), Hex(expected).c_str());
    g_failures++;
}

std::vector<uint8_t> Leb128(uint32_t value) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.PutLeb128(value);
    return bytes;
}

}  // namespace

int main() {
    Expect("leb128(0)", Leb128(0), {0x00});
    Expect("leb128(127)", Leb128(127), {0x7f});
    Expect("leb128(128)", Leb128(128), {0x80, 0x01});
    Expect("leb128(300)", Leb128(300), {0xac, 0x02});
    Expect("leb128(16384)", Leb128(16384), {0x80, 0x80, 0x01});
    Expect("leb128(0xffffffff)", Leb128(0xffffffffu), {0xff, 0xff, 0xff, 0xff, 0x0f});

    {
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.PutBits(0x1234, 16);
        Expect("f(16) 0x1234", bytes, {0x12, 0x34});
    }
    {
        // A frame id of 15 bits after show_existing_frame and frame_to_show_map_idx, then
        // trailing_bits: 1 | 101 | 010100001100101 | 1 0000.
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.PutBits(1, 1);
        writer.PutBits(5, 3);
        writer.PutBits(0x2865, 15);
        writer.PutTrailingBits();
        Expect("f(1) f(3) f(15) trailing_bits", bytes, {0xd5, 0x0c, 0xb0});
    }
    {
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.PutBits(0, 1);
        writer.PutBits(3, 4);
        writer.PutBits(0, 1);
        writer.PutBits(1, 1);
        writer.PutBits(0, 1);
        writer.PutLeb128(200);
        Expect("frame header OBU header, obu_size 200", bytes, {0x1a, 0xc8, 0x01});
    }

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
