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

#include "VkVideoEncoder/VkVideoEncoderHdrMetadata.h"

#include <cstring>
#include <vector>

namespace {

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xFF));
}

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)(x & 0xFF));
}

// AV1 leb128(), 7 payload bits per byte, low group first, continuation in
// bit 7. Every length here is far below 128 so this emits one byte in
// practice; it is written in full because a hand-rolled "just one byte" is
// how a 128-byte payload silently becomes a corrupt stream.
void PutLeb128(std::vector<uint8_t>& v, uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        v.push_back(byte);
    } while (value != 0);
}

// ST 2086 chromaticity (x50000) -> AV1 0.16 fixed point (x65536).
// Rounded to nearest; clamped because coordinate 1.0 is 50000 in ST 2086 and
// 65536 in 0.16, and 65536 does not fit the f(16) field. Real primaries are
// well under 0.8 so the clamp is a guard, not a path.
uint16_t St2086ChromaToAv1(uint16_t v)
{
    const uint64_t scaled = (((uint64_t)v * 65536ull) + 25000ull) / 50000ull;
    return (scaled > 0xFFFFull) ? (uint16_t)0xFFFF : (uint16_t)scaled;
}

// ST 2086 luminance (x10000 cd/m^2) -> AV1 luminance_max, 24.8 fixed point.
uint32_t St2086LumaMaxToAv1(uint32_t v)
{
    const uint64_t scaled = (((uint64_t)v * 256ull) + 5000ull) / 10000ull;
    return (scaled > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)scaled;
}

// ST 2086 luminance (x10000 cd/m^2) -> AV1 luminance_min, 18.14 fixed point.
uint32_t St2086LumaMinToAv1(uint32_t v)
{
    const uint64_t scaled = (((uint64_t)v * 16384ull) + 5000ull) / 10000ull;
    return (scaled > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)scaled;
}

size_t Emit(const std::vector<uint8_t>& bytes, uint8_t* out, size_t capacity,
            bool* outTruncated)
{
    if (outTruncated != nullptr) {
        *outTruncated = false;
    }
    if (bytes.empty()) {
        return 0;
    }
    if (bytes.size() > capacity) {
        if (outTruncated != nullptr) {
            *outTruncated = true;
        }
        return 0;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    return bytes.size();
}

}  // namespace

size_t VkEncBuildH265HdrSeiNal(const EncoderHdrStaticMetadata& metadata,
                               uint8_t* out, size_t capacity,
                               bool* outTruncated)
{
    if (outTruncated != nullptr) {
        *outTruncated = false;
    }
    if (!metadata.Any() || (out == nullptr)) {
        return 0;
    }

    // nal_unit_header(): forbidden_zero_bit 0, nal_unit_type 39
    // (PREFIX_SEI_NUT), nuh_layer_id 0, nuh_temporal_id_plus1 1. The two
    // bytes are part of the emulation-prevention scan window, so they go into
    // the same buffer rather than being written around it.
    std::vector<uint8_t> nal;
    nal.push_back((uint8_t)(39 << 1));   // 0x4E
    nal.push_back(0x01);

    // sei_message() is { payloadType, payloadSize, payload }, each of the two
    // sizes ff-coded. Both types (137, 144) and both sizes (24, 4) are below
    // 255, so each is one byte -- asserted by construction here rather than
    // by a chained-0xFF loop that could never run.
    if (metadata.masteringDisplayPresent != 0) {
        nal.push_back(137);   // mastering_display_colour_volume
        nal.push_back(24);
        // D.3.28 interleaves the pairs: (x[c], y[c]) for c = 0,1,2, i.e.
        // green, blue, red -- NOT all three x then all three y.
        for (int c = 0; c < 3; c++) {
            PutU16(nal, metadata.displayPrimaryX[c]);
            PutU16(nal, metadata.displayPrimaryY[c]);
        }
        PutU16(nal, metadata.whitePointX);
        PutU16(nal, metadata.whitePointY);
        PutU32(nal, metadata.maxDisplayMasteringLuminance);
        PutU32(nal, metadata.minDisplayMasteringLuminance);
    }
    if (metadata.contentLightLevelPresent != 0) {
        nal.push_back(144);   // content_light_level_info
        nal.push_back(4);
        PutU16(nal, metadata.maxContentLightLevel);
        PutU16(nal, metadata.maxFrameAverageLightLevel);
    }
    nal.push_back(0x80);      // rbsp_trailing_bits()

    // Byte stream NAL unit: a 4-byte start code -- the same length the driver
    // writes ahead of VPS/SPS/PPS in this buffer -- then the NAL with
    // emulation_prevention_three_byte inserted wherever two zero bytes are
    // followed by 0x00..0x03. min_display_mastering_luminance is very often
    // a small value like 1 (0x00000001), so this is a live path and not a
    // formality.
    std::vector<uint8_t> stream;
    stream.push_back(0x00);
    stream.push_back(0x00);
    stream.push_back(0x00);
    stream.push_back(0x01);
    int zeroRun = 0;
    for (size_t i = 0; i < nal.size(); i++) {
        const uint8_t b = nal[i];
        if ((zeroRun >= 2) && (b <= 0x03)) {
            stream.push_back(0x03);
            zeroRun = 0;
        }
        stream.push_back(b);
        zeroRun = (b == 0x00) ? (zeroRun + 1) : 0;
    }

    return Emit(stream, out, capacity, outTruncated);
}

size_t VkEncBuildAv1HdrMetadataObus(const EncoderHdrStaticMetadata& metadata,
                                    uint8_t* out, size_t capacity,
                                    bool* outTruncated)
{
    if (outTruncated != nullptr) {
        *outTruncated = false;
    }
    if (!metadata.Any() || (out == nullptr)) {
        return 0;
    }

    // obu_header(): obu_forbidden_bit 0, obu_type OBU_METADATA (5),
    // obu_extension_flag 0, obu_has_size_field 1, obu_reserved_1bit 0
    //   0 0101 0 1 0  ->  0x2A
    // The size field is set because these OBUs are appended to a stream whose
    // other OBUs carry one; a length-delimited container is not available on
    // the capture path this library takes.
    const uint8_t kObuMetadataHeader = 0x2A;

    std::vector<uint8_t> obus;

    if (metadata.masteringDisplayPresent != 0) {
        std::vector<uint8_t> payload;
        PutLeb128(payload, 2);   // METADATA_TYPE_HDR_MDCV
        // AV1 ORDERS THE PRIMARIES RED, GREEN, BLUE -- NOT ST 2086's GREEN,
        // BLUE, RED. This is the sharpest trap in the file: the two payloads
        // carry the same three numbers and permute them differently, so a
        // straight copy produces a stream that parses, validates, and names
        // green as red.
        //
        static const int kSt2086IndexForAv1[3] = {2, 0, 1};  // R, G, B
        for (int i = 0; i < 3; i++) {
            const int c = kSt2086IndexForAv1[i];
            PutU16(payload, St2086ChromaToAv1(metadata.displayPrimaryX[c]));
            PutU16(payload, St2086ChromaToAv1(metadata.displayPrimaryY[c]));
        }
        PutU16(payload, St2086ChromaToAv1(metadata.whitePointX));
        PutU16(payload, St2086ChromaToAv1(metadata.whitePointY));
        PutU32(payload,
               St2086LumaMaxToAv1(metadata.maxDisplayMasteringLuminance));
        PutU32(payload,
               St2086LumaMinToAv1(metadata.minDisplayMasteringLuminance));
        // trailing_bits(): the payload is byte-aligned, so this is one byte
        // with the stop bit set. It is REQUIRED -- metadata_obu() ends with
        // trailing_bits() whenever obu_has_size_field is 1.
        payload.push_back(0x80);

        obus.push_back(kObuMetadataHeader);
        PutLeb128(obus, payload.size());
        obus.insert(obus.end(), payload.begin(), payload.end());
    }

    if (metadata.contentLightLevelPresent != 0) {
        std::vector<uint8_t> payload;
        PutLeb128(payload, 1);   // METADATA_TYPE_HDR_CLL
        PutU16(payload, metadata.maxContentLightLevel);
        PutU16(payload, metadata.maxFrameAverageLightLevel);
        payload.push_back(0x80);

        obus.push_back(kObuMetadataHeader);
        PutLeb128(obus, payload.size());
        obus.insert(obus.end(), payload.begin(), payload.end());
    }

    return Emit(obus, out, capacity, outTruncated);
}
