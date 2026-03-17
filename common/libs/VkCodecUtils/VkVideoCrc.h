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

#ifndef _VKCODECUTILS_VKVIDEOCRC_H_
#define _VKCODECUTILS_VKVIDEOCRC_H_

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstdio>

/**
 * @brief Shared CRC calculator for encoder and decoder.
 * Holds stream (running) CRC state, per-frame CRC output, and owns the CRC output file.
 * SignalFrameEnd() writes the per-frame CRC line when a frame is complete.
 */
class VkVideoCrc {
public:
    VkVideoCrc() = default;

    /**
     * Begin CRC calculation: set init values and attach CRC text output.
     * If crcInitValue is empty, returns true and leaves CRC disabled.
     * If crcOutputFileName is non-empty, opens that file for writing; if empty, CRC text (per-frame and/or
     * stream summary from EndCrcCalculation) goes to stdout.
     * On fopen failure, prints a warning to stderr and returns false.
     * @param perFrameCrc When true, SignalFrameEnd writes "CRC Frame[N]:..." lines to the CRC stream.
     */
    bool BeginCrcCalculation(const std::vector<uint32_t>& crcInitValue,
                             bool perFrameCrc = false,
                             const std::string& crcOutputFileName = std::string());

    /** True when CRC is active (BeginCrcCalculation was called with non-empty init). */
    bool Enabled() const { return !m_accumulatedCrc.empty(); }

    /** End CRC calculation: optionally write stream "CRC: 0x...\n", close file, clear state. */
    void EndCrcCalculation(bool writeStreamCrcFirst = false);

    /** Update stream and current-frame CRC with this chunk. Call in the same order as data is written (e.g. from WriteDataToFile). */
    void UpdateCrc(const uint8_t* data, size_t size);

    /** Signal that a frame is complete; writes "CRC Frame[N]: 0x...\n" to the CRC file from data already captured in UpdateCrc. */
    void SignalFrameEnd(uint32_t frameIndex);

    /** Copy out current running CRC values. Returns 0 if not enabled or pCrcValues is null. */
    size_t GetCrcValues(uint32_t* pCrcValues, size_t buffSize) const;

    /** End CRC calculation and clear state. Same as EndCrcCalculation(false). */
    void Deinit();

    /**
     * IEEE CRC-32 (same polynomial as stream/frame CRC): fold bytes into *checksum.
     * Any of checksum, data null, or length 0 is a no-op.
     */
    static void AccumulateCrc32(uint32_t* checksum, const uint8_t* data, size_t length);

private:
    std::vector<uint32_t> m_initValue;
    std::vector<uint32_t> m_accumulatedCrc;   /* stream CRC (all data so far) */
    std::vector<uint32_t> m_currentFrameCrc;  /* current frame CRC, reset at SignalFrameEnd */
    FILE*                 m_file = nullptr;
    bool                  m_perFrameCrcEnabled = true;  /* when true, SignalFrameEnd writes "CRC Frame[N]:..." lines */
};

#endif /* _VKCODECUTILS_VKVIDEOCRC_H_ */
