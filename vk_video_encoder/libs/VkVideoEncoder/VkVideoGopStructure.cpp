/*
 * Copyright 2024 NVIDIA Corporation.
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

#include "VkVideoGopStructure.h"

VkVideoGopStructure::VkVideoGopStructure(int8_t gopFrameCount,
                                         int8_t idrPeriod,
                                         int8_t consecutiveBFrameCount,
                                         int8_t temporalLayerCount,
                                         FrameType lastFrameType)
    : m_gopFrameCount(gopFrameCount)
    , m_idrPeriod(idrPeriod)
    , m_consecutiveBFrameCount(consecutiveBFrameCount)
    , m_gopFrameCycle(m_consecutiveBFrameCount + 1)
    , m_temporalLayerCount(temporalLayerCount)
    , m_lastFrameType(lastFrameType)
{
    Init();
}

bool VkVideoGopStructure::Init()
{
    m_gopFrameCycle = m_consecutiveBFrameCount + 1;
    // Map display order to decode order
    ComputeDecodeOrderMap();
    return true;
}

void VkVideoGopStructure::ComputeDecodeOrderMap()
{
    m_decodeOrderMap.resize(m_gopFrameCount + 1);
    int8_t decodeIndex = 0;
    int8_t gopFrameNum = 0;

    // First frame is always a reference and
    // it's decode order is the same as the display order.
    m_decodeOrderMap[gopFrameNum].frameType   = GetFrameType(0);
    m_decodeOrderMap[gopFrameNum].decodeOrder = decodeIndex++;
    m_decodeOrderMap[gopFrameNum].isReference = 1;
    gopFrameNum++;
    for ( ;gopFrameNum < m_gopFrameCount + 1;
            gopFrameNum += m_gopFrameCycle) {

        // First, assign decode order for I and P frames
        for (int i = 0; i < m_gopFrameCycle; ++i) {
            FrameType frameType = GetFrameType(gopFrameNum + i);
            if ((frameType >= FRAME_TYPE_I) || (frameType == FRAME_TYPE_P)) {
                m_decodeOrderMap[gopFrameNum + i].frameType   = frameType;
                m_decodeOrderMap[gopFrameNum + i].isReference = 1;
                m_decodeOrderMap[gopFrameNum + i].decodeOrder = decodeIndex++;
            }
        }

        // Then, assign decode order for B frames
        for (int i = 0; i < m_gopFrameCycle; ++i) {
            FrameType frameType = GetFrameType(gopFrameNum + i);
            if (frameType == FRAME_TYPE_B) {
                m_decodeOrderMap[gopFrameNum + i].frameType   = frameType;
                m_decodeOrderMap[gopFrameNum + i].isReference = 0;
                m_decodeOrderMap[gopFrameNum + i].decodeOrder = decodeIndex++;
            }
        }
    }

    const bool verboseGopStructure = false;
    if (verboseGopStructure) {
        std::cout << "gopFrameCount: " << (uint32_t)m_gopFrameCount
                  << ", idrPeriod: " << (uint32_t)m_idrPeriod
                  << ", consecutiveBFrameCount: " << (uint32_t)m_consecutiveBFrameCount
                  << ", gopFrameCycle: " << (uint32_t)m_gopFrameCycle << std::endl;

        for (gopFrameNum = 0; gopFrameNum < m_gopFrameCount + 1; gopFrameNum++) {
            std::cout <<  (uint32_t)gopFrameNum << " "
                       << (uint32_t)m_decodeOrderMap[gopFrameNum].decodeOrder << " "
                       << GetFrameTypeName(m_decodeOrderMap[gopFrameNum].frameType) << std::endl;
        }
        std::cout << std::endl;
    }
}

VkVideoGopStructure::FrameType VkVideoGopStructure::GetFrameType(uint64_t frameNumInInputOrder,
                                                                 bool firstFrame, bool lastFrame) const
{
    if (firstFrame) {
        return FRAME_TYPE_IDR;
    }

    if (lastFrame) {
        return m_lastFrameType;
    }

    if (frameNumInInputOrder % m_idrPeriod == 0) {
        return FRAME_TYPE_IDR;
    }

    if (frameNumInInputOrder % m_gopFrameCount == 0) {
        return FRAME_TYPE_I;
    }

    uint8_t posInGop = frameNumInInputOrder % m_gopFrameCount;
    return (posInGop % (m_consecutiveBFrameCount + 1) == 0) ? FRAME_TYPE_P : FRAME_TYPE_B;
}

void VkVideoGopStructure::PrintGopStructure(uint64_t numFrames) const
{
    const bool exactGop = (numFrames == uint64_t(-1));
    numFrames = exactGop ? m_gopFrameCount : numFrames;

    std::cout << std::endl << "Display order: ";
    for (uint64_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(3) << i << " ";
    }
    std::cout << std::endl << "Frame Type:    ";

    for (uint64_t i = 0; i < (numFrames - 1); i++) {
        std::cout << std::setw(3) << GetFrameTypeName(GetFrameType(i)) << " ";
    }
    std::cout << std::setw(3) << GetFrameTypeName(GetFrameType(numFrames - 1, false, exactGop ? false : true));

    std::cout << std::endl << "Decode  order: ";

    for (uint64_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(3) << (uint32_t)GetFrameDecodeOrderPosition(i) << " ";
    }

    std::cout << std::endl;
}

void VkVideoGopStructure::VisitGopFrames(int8_t gopNum,
                                         const std::function<void(int8_t, FrameType)>& callback,
                                         bool searchBackward, bool searchForward) const
{
    if ((gopNum < 0) || (gopNum >= m_gopFrameCount)) {
        return;
    }

    FrameType frameType = GetFrameType(gopNum);
    callback(gopNum, frameType);

    // For I-frames, no further references are needed
    if (frameType >= FRAME_TYPE_I) {
        return;
    }

    // For P-frames, search backward for the nearest I-frame or P-frame, then stop
    if ((frameType == FRAME_TYPE_P) && searchBackward) {
        VisitGopFrames(gopNum - 1, callback, true, false);
        return; // Stop after processing the first reference
    }

    // For B-frames, accumulate references by checking frames before and after
    if (frameType == FRAME_TYPE_B) {
        if (searchBackward) {
            VisitGopFrames(gopNum - 1, callback, true, false);
        }
        if (searchForward) {
            VisitGopFrames(gopNum + 1, callback, false, true);
        }
    }
}

uint8_t VkVideoGopStructure::GetReferences(int gopNum, std::bitset<64>& refMask) const
{
    uint8_t refCount = 0;
    VisitGopFrames(gopNum, [&refMask, gopNum, &refCount](int8_t visitedFrame, FrameType frameType) {
        if (((frameType >= FRAME_TYPE_I) || (frameType == FRAME_TYPE_P)) && (visitedFrame != gopNum)) { // Exclude self-references
            refMask.set(visitedFrame);
            refCount++;
        }
    });
    return refCount;
}

uint8_t VkVideoGopStructure::GetReferenceNumbers(int8_t gopNum, std::vector<int8_t>& refNumbers,
                                                 bool searchBackward, bool searchForward) const
{
    refNumbers.clear();
    uint8_t refCount = 0;
    VisitGopFrames(gopNum, [&refNumbers, gopNum, &refCount](int visitedFrame, FrameType frameType) {
        if (((frameType >= FRAME_TYPE_I) || (frameType == FRAME_TYPE_P)) &&
                (visitedFrame != gopNum)) { // Exclude self-references
            refNumbers.push_back(visitedFrame);
            refCount++;
        }
    }, searchBackward, searchForward);
    return refCount;
}

void VkVideoGopStructure::DumpFrameGopStructure(uint64_t frameNum,
                                                bool firstFrame, bool lastFrame) const
{
    uint8_t frameNumInDisplayOrder = frameNum % m_idrPeriod;
    FrameType frameType = FRAME_TYPE_INVALID;
    int posInGOP = GetPositionInGOP(frameNumInDisplayOrder, frameType);
    uint8_t decodeOrder = GetFrameDecodeOrderPosition(frameNumInDisplayOrder);
    std::bitset<64> references = std::bitset<64>();
    // GetReferences(frameNumInDisplayOrder % gopFrameCount, frameNumInDisplayOrder % gopFrameCount, references);
    uint32_t refCount = GetReferences(frameNumInDisplayOrder % m_gopFrameCount, references);
    std::vector<int8_t> refNumbers;
    GetReferenceNumbers(frameNumInDisplayOrder % m_gopFrameCount, refNumbers);

    std::vector<int8_t> forwardRefNumbers;
    GetReferenceNumbers(frameNumInDisplayOrder % m_gopFrameCount, forwardRefNumbers, false, true);

    std::vector<int8_t> backwardsRefNumbers;
    GetReferenceNumbers(frameNumInDisplayOrder % m_gopFrameCount, backwardsRefNumbers, true, false);


    std::cout << "\t" << frameNumInDisplayOrder   << ", "
              << "\t" << (uint32_t)decodeOrder   << ", "
              << "\t" << posInGOP   << ", "
              << "\t" << GetFrameTypeName(frameType)  << ", "
              << "\t" << refCount  << ": "
              << " " << references << " ::: ";

    for (size_t i = 0; i < refNumbers.size(); i++) {
        std::cout << (uint32_t)refNumbers[i] << ", ";
    }

    if (forwardRefNumbers.size() != 0) {
        std::cout << " :: FWD :: ";

        for (size_t i = 0; i < forwardRefNumbers.size(); i++) {
            std::cout << (uint32_t)forwardRefNumbers[i] << ", ";
        }
    }

    if (backwardsRefNumbers.size() != 0) {
        std::cout << " :: BWD :: ";

        for (size_t i = 0; i < backwardsRefNumbers.size(); i++) {
            std::cout << (uint32_t)backwardsRefNumbers[i] << ", ";
        }
    }
    std::cout << std::endl;
}

int TestGopStructure()
{
    int8_t gopFrameCount = 8;
    int8_t idrPeriod = 16;
    int8_t consecutiveBFrameCount = 3;
    int8_t temporalLayerCount = 3; // Not used in this example
    VkVideoGopStructure::FrameType lastFrameType = VkVideoGopStructure::FRAME_TYPE_P;

    VkVideoGopStructure gop(gopFrameCount, idrPeriod, consecutiveBFrameCount, temporalLayerCount, lastFrameType);

    std::cout << "GOP structure:";
    gop.PrintGopStructure(12);

    std::cout << "Frame Display Order, Frame Decode Order, Position in GOP, Frame Type,  Reference Bitmask" << std::endl;

    const int maxFrames = 34;
    for (uint64_t frameNumInDisplayOrder = 0; frameNumInDisplayOrder < (maxFrames - 1); ++frameNumInDisplayOrder) {
    // Print GOP structure for a certain number of frames
        gop.DumpFrameGopStructure(frameNumInDisplayOrder); // Example: print for first maxFrames - 1 frames
    }

    gop.DumpFrameGopStructure((maxFrames - 1), true);
    return 0;
}
