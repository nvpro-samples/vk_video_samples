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

// Writes the bitstream syntax the crafted parser cases build: H.264 and H.265 RBSPs, which
// AnnexBNal() wraps in Annex B NAL units with emulation prevention, AV1 OBU payloads, which Obu()
// wraps, and VP9 frame headers.

#ifndef PARSER_TRACE_BIT_WRITER_H
#define PARSER_TRACE_BIT_WRITER_H

#include <cstdint>
#include <vector>

namespace parser_trace {

class BitWriter {
   public:
    // u(n) and f(n): the low bits of value, most significant first. bits is at most 64.
    BitWriter& U(uint32_t bits, uint64_t value) {
        for (uint32_t i = bits; i > 0; i--) {
            Bit(static_cast<uint32_t>((value >> (i - 1)) & 1u));
        }
        return *this;
    }

    BitWriter& Flag(bool value) { return U(1, value ? 1 : 0); }

    // count zero bits: the prefix of an Exp-Golomb or uvlc code that no value fits.
    BitWriter& Zeros(uint32_t count) { return U(count, 0); }

    // ue(v), H.264 9.1 and H.265 9.2. 2^32 - 1, for example, takes 32 leading zeros.
    BitWriter& Ue(uint64_t value) {
        const uint64_t codeNum = value + 1;
        uint32_t leadingZeros = 0;
        while ((codeNum >> (leadingZeros + 1)) != 0) {
            leadingZeros++;
        }
        return Zeros(leadingZeros).U(leadingZeros + 1, codeNum);
    }

    // se(v): 0, 1, -1, 2, -2, ...
    BitWriter& Se(int64_t value) {
        return Ue((value > 0) ? (2 * static_cast<uint64_t>(value) - 1) : (2 * static_cast<uint64_t>(-value)));
    }

    // AV1 uvlc() (4.10.3): values below 2^32 - 1.
    BitWriter& Uvlc(uint64_t value) { return Ue(value); }

    // rbsp_trailing_bits(), and AV1 trailing_bits() of a byte-aligned payload: a one bit, then zero
    // bits to the byte boundary.
    BitWriter& TrailingBits() {
        Bit(1);
        return AlignZero();
    }

    // Zero bits to the byte boundary: VP9 trailing_bits(), AV1 byte_alignment().
    BitWriter& AlignZero() {
        while (m_bitsInLastByte != 0) {
            Bit(0);
        }
        return *this;
    }

    // Whole bytes, after the writer is byte-aligned.
    BitWriter& Bytes(const std::vector<uint8_t>& bytes) {
        AlignZero();
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
        return *this;
    }

    const std::vector<uint8_t>& Data() const { return m_bytes; }

   private:
    void Bit(uint32_t bit) {
        if (m_bitsInLastByte == 0) {
            m_bytes.push_back(0);
        }
        m_bytes.back() |= static_cast<uint8_t>(bit << (7 - m_bitsInLastByte));
        m_bitsInLastByte = (m_bitsInLastByte + 1) % 8;
    }

    std::vector<uint8_t> m_bytes;
    uint32_t m_bitsInLastByte = 0;
};

// An H.264 or H.265 NAL unit in Annex B: a four-byte start code, the NAL unit header, and the RBSP
// with an emulation prevention byte after every two zero bytes that a byte of 3 or less follows
// (H.264 7.4.1, H.265 7.4.2).
inline std::vector<uint8_t> AnnexBNal(const std::vector<uint8_t>& header, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> nal = {0, 0, 0, 1};
    nal.insert(nal.end(), header.begin(), header.end());
    uint32_t zeros = 0;
    for (uint8_t byte : rbsp) {
        if ((zeros >= 2) && (byte <= 3)) {
            nal.push_back(3);
            zeros = 0;
        }
        nal.push_back(byte);
        zeros = (byte == 0) ? (zeros + 1) : 0;
    }
    return nal;
}

// An AV1 OBU with obu_has_size_field set and no extension (AV1 5.3).
inline std::vector<uint8_t> Obu(uint32_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> obu = {static_cast<uint8_t>((type << 3) | 0x02)};
    uint64_t size = payload.size();
    do {
        const uint8_t byte = size & 0x7f;
        size >>= 7;
        obu.push_back(byte | ((size != 0) ? 0x80 : 0));
    } while (size != 0);
    obu.insert(obu.end(), payload.begin(), payload.end());
    return obu;
}

}  // namespace parser_trace

#endif  // PARSER_TRACE_BIT_WRITER_H
