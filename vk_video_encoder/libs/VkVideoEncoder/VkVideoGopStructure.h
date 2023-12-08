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

#ifndef _VKVIDEOENCODER_VKVIDEOGOPSTRUCTURE_H_
#define _VKVIDEOENCODER_VKVIDEOGOPSTRUCTURE_H_

#include <assert.h>
#include <stdint.h>
#include <atomic>
#include <bitset>
#include <vector>
#include <functional>
#include <iostream>
#include <iomanip>

static const uint32_t MAX_GOP_SIZE = 64;

class VkVideoGopStructure {

public:
    enum FrameType { FRAME_TYPE_P = 0, FRAME_TYPE_B = 1, FRAME_TYPE_I = 2,
                     FRAME_TYPE_IDR = 3, FRAME_TYPE_INTRA_REFRESH = 6, FRAME_TYPE_INVALID = -1 };

    struct GopEntry {
      FrameType                 frameType;
      uint8_t                   decodeOrder;
      uint8_t                   isReference : 1;
      std::bitset<MAX_GOP_SIZE> references;
    };

    VkVideoGopStructure(int8_t gopFrameCount = 8,
                        int8_t idrPeriod = 16,
                        int8_t consecutiveBFrameCount = 2,
                        int8_t temporalLayerCount = 1,
                        FrameType lastFrameType = FRAME_TYPE_P);

    bool Init();

    static const char* GetFrameTypeName(FrameType frameType)
    {
        switch (frameType){
        case FRAME_TYPE_P:
            return "P";
        case FRAME_TYPE_B:
            return "B";
        case FRAME_TYPE_I:
            return "I";
        case FRAME_TYPE_IDR:
            return "IDR";
        case FRAME_TYPE_INTRA_REFRESH:
            return "INTRA_REFRESH";
        default:
            break;
        }

        return "UNDEFINED";
    }

    // gopFrameCount is the number of frames within a group of pictures (GOP)
    // intended to be used by the application.
    // If it is set to 0, the rate control algorithm may assume an
    // implementation-dependent GOP length. If it is set to UINT32_MAX,
    // the GOP length is treated as infinite.
    void SetGopFrameCount(int8_t gopFrameCount) { m_gopFrameCount = gopFrameCount; }
    int8_t GetGopFrameCount() const { return m_gopFrameCount; }

    // idrPeriod is the interval, in terms of number of frames, between two IDR frames (see IDR period).
    // If it is set to 0, the rate control algorithm may assume an implementation-dependent IDR period.
    // If it is set to UINT8_MAX, the IDR period is treated as infinite.
    void SetIdrPeriod(int8_t idrPeriod) { m_idrPeriod = idrPeriod; }
    int8_t GetIdrPeriod() const { return m_idrPeriod; }

    // consecutiveBFrameCount is the number of consecutive B frames between I and/or P frames within the GOP.
    void SetConsecutiveBFrameCount(int8_t consecutiveBFrameCount) { m_consecutiveBFrameCount = consecutiveBFrameCount; }
    int8_t GetConsecutiveBFrameCount() const { return m_consecutiveBFrameCount; }

    // specifies the number of H.264/5 sub-layers that the application intends to use.
    void SetTemporalLayerCount(int8_t temporalLayerCount) { m_temporalLayerCount = temporalLayerCount; }
    int8_t GetTemporalLayerCount() const { return m_temporalLayerCount; }

    // lastFrameType is the type of frame that will be used for the last frame in the stream.
    // This frame type will replace the type regardless on the type determined by the GOP structure.
    bool SetLastFrameType(FrameType lastFrameType) {

        m_lastFrameType = lastFrameType;
        return true;
    }

    virtual FrameType GetFrameType(uint64_t frameNumInDisplayOrder,
                                   bool firstFrame = false, bool lastFrame = false) const;

    virtual void PrintGopStructure(uint64_t numFrames = uint64_t(-1)) const;

    virtual void VisitGopFrames(int8_t gopNum,
                                const std::function<void(int8_t, FrameType)>& callback,
                                bool searchBackward = true, bool searchForward = true) const;

    virtual uint8_t GetReferences(int gopNum, std::bitset<64>& refMask) const;
    virtual uint8_t GetReferenceNumbers(int8_t gopNum, std::vector<int8_t>& refNumbers,
                                        bool searchBackward = true, bool searchForward = true) const;


    uint8_t GetPositionInGOP(uint8_t& positionInGopInDisplayOrder, FrameType& frameType,
                             bool firstFrame = false, bool lastFrame = false) const {

        frameType = GetFrameType(positionInGopInDisplayOrder, firstFrame, lastFrame);
        if (frameType >= FRAME_TYPE_IDR) {
            positionInGopInDisplayOrder = 1; // next frame
            return 0;
        }

        uint8_t currentPositionInGop = positionInGopInDisplayOrder++;

        return currentPositionInGop % m_idrPeriod;
    }

    uint8_t GetFrameDecodeOrderPosition(uint64_t frameNumInDisplayOrder, bool useGopFrameCountPeriod = false) const {

        uint8_t positionInGopInDecodeOrder = m_decodeOrderMap[frameNumInDisplayOrder % m_gopFrameCount].decodeOrder;
        if (useGopFrameCountPeriod) {
            return positionInGopInDecodeOrder;
        }
        uint8_t gopIndex = uint8_t(frameNumInDisplayOrder / m_idrPeriod);
        uint8_t baseDecodeOrder = gopIndex * m_idrPeriod;
        return baseDecodeOrder + positionInGopInDecodeOrder;
    }

    uint64_t GetFrameInDecodeOrder(uint64_t frameNumInDisplayOrder) const {
            uint8_t positionInGOP = GetFrameDecodeOrderPosition(frameNumInDisplayOrder);
            uint64_t gopIndex = frameNumInDisplayOrder / m_gopFrameCount;
            uint64_t baseDecodeOrder = gopIndex * m_gopFrameCount;
            return baseDecodeOrder + positionInGOP;
    }

    bool IsFrameReference(uint64_t frameNumInDisplayOrder) const {
        return (m_decodeOrderMap[frameNumInDisplayOrder % m_gopFrameCount].isReference == 1);
    }

    virtual void DumpFrameGopStructure(uint64_t frameNumInInputOrder,
                                       bool firstFrame = false, bool lastFrame = false) const;

    virtual ~VkVideoGopStructure() {

    }

protected:
    virtual void ComputeDecodeOrderMap();
private:
    int8_t                m_gopFrameCount;
    int8_t                m_idrPeriod;
    int8_t                m_consecutiveBFrameCount;
    int8_t                m_gopFrameCycle;
    int8_t                m_temporalLayerCount;
    FrameType             m_lastFrameType;
    std::vector<GopEntry> m_decodeOrderMap;
};
#endif /* _VKVIDEOENCODER_VKVIDEOGOPSTRUCTURE_H_ */
