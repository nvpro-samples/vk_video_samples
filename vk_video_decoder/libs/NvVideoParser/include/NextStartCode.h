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

#ifndef _NEXTSTARTCODE_H_
#define _NEXTSTARTCODE_H_

#include <cstddef>
#include <cstdint>

// The start code scanners. Each returns how many bytes of pdatain[0, datasize) it consumed: up to
// and including the first 00 00 01 (found_start_code is then true), or all of them. bitBfr holds the
// last bytes scanned, so that a start code split across two calls is found.
//
// Each scanner but the C one is built with its instruction set's flags, so its file defines the
// scanner and nothing else, and includes only this header and the intrinsics. Code it shared with the
// rest of the parser, an inline function or a template, would be built with those flags too, and the
// linker may keep that copy for every caller: an AVX instruction on a CPU without AVX.
// VulkanVideoDecoder::ParseByteStream() picks one by the CPU's features.
typedef size_t (*NextStartCodeFunc)(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);

size_t NextStartCodeC(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
#if defined(__x86_64__) || defined(_M_X64)
size_t NextStartCodeSSSE3(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
size_t NextStartCodeAVX2(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
size_t NextStartCodeAVX512(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
#endif
#if defined(__aarch64__) || defined(__ARM_ARCH_7A__) || defined(_M_ARM64)
size_t NextStartCodeNEON(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
#endif
#if defined(__aarch64__)
size_t NextStartCodeSVE(const uint8_t* pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code);
#endif

#endif /* _NEXTSTARTCODE_H_ */
