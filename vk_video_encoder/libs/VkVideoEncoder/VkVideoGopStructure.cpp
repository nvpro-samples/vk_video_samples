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

VkVideoGopStructure::VkVideoGopStructure(uint8_t gopFrameCount,
                                         int32_t idrPeriod,
                                         uint8_t consecutiveBFrameCount,
                                         uint8_t temporalLayerCount,
                                         FrameType lastFrameType,
                                         FrameType preIdrAnchorFrameType,
                                         bool closedGop)
    : m_gopFrameCount(gopFrameCount)
    , m_consecutiveBFrameCount(consecutiveBFrameCount)
    , m_gopFrameCycle((uint8_t)(m_consecutiveBFrameCount + 1))
    , m_temporalLayerCount(temporalLayerCount)
    , m_idrPeriod(idrPeriod)
    , m_lastFrameType(lastFrameType)
    , m_preClosedGopAnchorFrameType(preIdrAnchorFrameType)
    , m_closedGop(closedGop)
{
    Init(uint64_t(-1));
}

bool VkVideoGopStructure::Init(uint64_t maxNumFrames)
{
    m_gopFrameCycle = (uint8_t)(m_consecutiveBFrameCount + 1);
    m_gopFrameCount = (uint8_t)std::min<uint64_t>(m_gopFrameCount, maxNumFrames);
    if (m_idrPeriod > 0) {
        m_idrPeriod = (uint32_t)std::min<uint64_t>(m_idrPeriod, maxNumFrames);
    }
    // Map display order to decode order
    return true;
}

void VkVideoGopStructure::PrintGopStructure(uint64_t numFrames) const
{
    std::cout << std::endl << "Input order:   ";
    for (uint64_t frameNum = 0; frameNum < numFrames; frameNum++) {
        std::cout << std::setw(3) << frameNum << " ";
    }
    std::cout << std::endl << "Frame Type:   ";

    GopState gopState;
    GopPosition gopPos(gopState.positionInInputOrder);
    for (uint64_t frameNum = 0; frameNum < (numFrames - 1); frameNum++) {

        GetPositionInGOP(gopState, gopPos);
        std::cout << std::setw(4) << GetFrameTypeName(gopPos.pictureType);
    }
    GetPositionInGOP(gopState, gopPos, false, true);
    std::cout << std::setw(4) << GetFrameTypeName(gopPos.pictureType);

    std::cout << std::endl << "Encode  order: ";

    gopState = GopState();
    for (uint64_t i = 0; i < (numFrames - 1); i++) {
        GetPositionInGOP(gopState, gopPos);
        std::cout << std::setw(3) << gopPos.encodeOrder << " ";
    }
    GetPositionInGOP(gopState, gopPos, false, true);
    std::cout << std::setw(3) << gopPos.encodeOrder << " ";

    std::cout << std::endl;
}

void VkVideoGopStructure::DumpFrameGopStructure(GopState& gopState,
                                                bool firstFrame, bool lastFrame) const
{
    GopPosition gopPos(gopState.positionInInputOrder);
    GetPositionInGOP(gopState, gopPos);

    std::cout << "  " << gopPos.inputOrder   << ", "
              << "\t" << gopPos.encodeOrder   << ", "
              << "\t" << (uint32_t)gopPos.inGop   << ", "
              << "\t" << GetFrameTypeName(gopPos.pictureType);

    std::cout << std::endl;
}

void VkVideoGopStructure::DumpFramesGopStructure(uint64_t firstFrameNumInInputOrder, uint64_t numFrames) const
{
    std::cout << "Input Encode Position  Frame " << std::endl;
    std::cout << "order order   in GOP   type  " << std::endl;
    const uint64_t lastFrameNumInInputOrder = firstFrameNumInInputOrder + numFrames - 1;
    GopState gopState;
    for (uint64_t frameNumInDisplayOrder = firstFrameNumInInputOrder; frameNumInDisplayOrder < lastFrameNumInInputOrder; ++frameNumInDisplayOrder) {
        // Print GOP structure for a number of frames
        DumpFrameGopStructure(gopState);
    }
    DumpFrameGopStructure(gopState, true);

}
