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

/*
 * Every start code scanner this CPU runs gives the C scanner's results over the same streams: the
 * same bytes consumed by each call, the same start codes found, and the same carried bytes (the low
 * 16 bits of bitBfr, the only ones the next call reads). Each stream is scanned in pieces of every
 * size from 1 to 300 bytes and of random sizes, as the parser hands them over, so start codes fall
 * across pieces and across the vector loops' tails. Each piece is copied to a buffer of exactly its
 * size, so a sanitizer build reports a scanner that reads past it.
 *
 * Exit codes: 0 pass, 1 fail.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "NextStartCode.h"
#include "cpudetect.h"

namespace {

struct Kernel {
    const char* name;
    NextStartCodeFunc scan;
};

struct Step {
    size_t consumed;
    bool found;
    uint32_t carried;

    bool operator==(const Step& other) const {
        return (consumed == other.consumed) && (found == other.found) && (carried == other.carried);
    }
};

// The scanners the parser can pick on this CPU (VulkanVideoDecoder::ParseByteStream()).
std::vector<Kernel> AvailableKernels() {
    std::vector<Kernel> kernels{{"C", NextStartCodeC}};
#if !defined(DISABLE_VK_VIDEO_PARSER_SIMD_OPTIMIZATIONS)
    // Unused where the parser has no SIMD kernel, such as 32-bit x86.
    [[maybe_unused]] const SIMD_ISA best = check_simd_support();
#if defined(__x86_64__) || defined(_M_X64)
    if ((best == SIMD_ISA::SSSE3) || (best == SIMD_ISA::AVX2) || (best == SIMD_ISA::AVX512)) {
        kernels.push_back({"SSSE3", NextStartCodeSSSE3});
    }
    if ((best == SIMD_ISA::AVX2) || (best == SIMD_ISA::AVX512)) {
        kernels.push_back({"AVX2", NextStartCodeAVX2});
    }
    if (best == SIMD_ISA::AVX512) {
        kernels.push_back({"AVX512", NextStartCodeAVX512});
    }
#elif defined(__aarch64__) || defined(__ARM_ARCH_7A__) || defined(_M_ARM64)
    if ((best == SIMD_ISA::NEON) || (best == SIMD_ISA::SVE)) {
        kernels.push_back({"NEON", NextStartCodeNEON});
    }
#if defined(__aarch64__)
    if (best == SIMD_ISA::SVE) {
        kernels.push_back({"SVE", NextStartCodeSVE});
    }
#endif
#endif
#endif
    return kernels;
}

// A stream with start codes and near misses (00 00 00 01, 00 00 02, 00 01) among random bytes, and
// runs of zeros, which a byte stream holds before a start code.
std::vector<uint8_t> MakeStream(std::mt19937& random, size_t size) {
    std::vector<uint8_t> stream(size);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> pick(0, 99);
    for (size_t i = 0; i < size; i++) {
        const int kind = pick(random);
        stream[i] = (uint8_t)((kind < 30) ? 0 : (kind < 40) ? 1 : (kind < 42) ? 2 : byte(random));
    }
    static const uint8_t kStartCode[] = {0, 0, 1};
    std::uniform_int_distribution<size_t> at(0, (size > 4) ? size - 4 : 0);
    for (int n = 0; n < 12; n++) {
        // A stream shorter than a start code gets the part that fits.
        const size_t offset = at(random);
        std::copy_n(kStartCode, std::min(sizeof(kStartCode), size - offset), stream.begin() + offset);
    }
    return stream;
}

// The whole stream scanned piece by piece, as the parser does: each call gets at most the next
// piece size, and stops early at a start code.
std::vector<Step> ScanStream(NextStartCodeFunc scan, const std::vector<uint8_t>& stream, const std::vector<size_t>& pieces) {
    std::vector<Step> steps;
    uint32_t bitBfr = ~0u;
    size_t position = 0;
    size_t call = 0;
    while (position < stream.size()) {
        const size_t size = std::min(pieces[call % pieces.size()], stream.size() - position);
        std::vector<uint8_t> piece(stream.begin() + position, stream.begin() + position + size);
        bool found = false;
        const size_t consumed = scan(piece.data(), size, bitBfr, found);
        steps.push_back({consumed, found, bitBfr & 0xFFFFu});
        if ((consumed == 0) || (consumed > size)) {
            break;
        }
        position += consumed;
        call++;
    }
    return steps;
}

}  // namespace

int main() {
    const std::vector<Kernel> kernels = AvailableKernels();
    std::mt19937 random(20261030);
    std::uniform_int_distribution<size_t> randomPiece(1, 700);

    uint32_t scans = 0;
    uint32_t mismatches = 0;
    uint64_t startCodes = 0;
    static const size_t kStreamSizes[] = {1, 2, 3, 31, 33, 64, 65, 127, 129, 255, 257, 1000, 4096, 9000};
    for (size_t streamSize : kStreamSizes) {
        const std::vector<uint8_t> stream = MakeStream(random, streamSize);
        std::vector<std::vector<size_t>> pieceSizes;
        for (size_t piece = 1; piece <= 300; piece++) {
            pieceSizes.push_back({piece});
        }
        for (int n = 0; n < 50; n++) {
            std::vector<size_t> sizes;
            for (int s = 0; s < 64; s++) {
                sizes.push_back(randomPiece(random));
            }
            pieceSizes.push_back(sizes);
        }
        for (const std::vector<size_t>& pieces : pieceSizes) {
            const std::vector<Step> expected = ScanStream(kernels[0].scan, stream, pieces);
            for (const Step& step : expected) {
                startCodes += step.found ? 1 : 0;
            }
            for (size_t k = 1; k < kernels.size(); k++) {
                const std::vector<Step> steps = ScanStream(kernels[k].scan, stream, pieces);
                scans++;
                if (steps == expected) {
                    continue;
                }
                if (mismatches++ < 10) {
                    size_t first = 0;
                    while ((first < steps.size()) && (first < expected.size()) && (steps[first] == expected[first])) {
                        first++;
                    }
                    std::printf("FAIL: %s differs from C on a %zu-byte stream, first piece %zu, at call %zu\n", kernels[k].name,
                                streamSize, pieces[0], first);
                }
            }
        }
    }

    std::printf("kernels:");
    for (const Kernel& kernel : kernels) {
        std::printf(" %s", kernel.name);
    }
    std::printf("; %u scans against C, %llu start codes found by C, %u mismatches\n", scans, (unsigned long long)startCodes,
                mismatches);
    if (mismatches != 0) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
