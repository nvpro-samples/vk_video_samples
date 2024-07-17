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


    enum Flags { FLAGS_IS_REF         = (1 << 0), // frame is a reference
                 FLAGS_CLOSE_GOP      = (1 << 1), // Last reference in the Gop. Indicates the end of a closed Gop.
                 FLAGS_NONUNIFORM_GOP = (1 << 2), // nonuniform  Gop part of sequence (usually used to terminate Gop).
               };

    struct GopState {
        uint32_t positionInInputOrder;
        uint32_t lastRefInInputOrder;
        uint32_t lastRefInEncodeOrder;

        GopState()
        : positionInInputOrder(0)
        , lastRefInInputOrder(0)
        , lastRefInEncodeOrder(0) {}
    };

    struct GopPosition {
        uint32_t   inputOrder;  // input order in the IDR sequence
        uint32_t   encodeOrder; // encode order in the Gop
        uint8_t    inGop;       // The position in Gop in input order
        int8_t     numBFrames;  // Number of B frames in this part of the Gop, -1 if not a B frame
        int8_t     bFramePos;   // The B position in Gop, -1 if not a B frame
        FrameType  pictureType;   // The type of the picture
        uint32_t   flags;       // one or multiple of flags of type Flags above

        GopPosition(uint32_t positionInGopInInputOrder)
        : inputOrder(positionInGopInInputOrder)
        , encodeOrder(0)
        , inGop(0)
        , numBFrames(-1)
        , bFramePos(-1)
        , pictureType(FRAME_TYPE_INVALID)
        , flags(0)
        {}
    };

    VkVideoGopStructure(uint8_t gopFrameCount = 8,
                        int32_t idrPeriod = 60,
                        uint8_t consecutiveBFrameCount = 2,
                        uint8_t temporalLayerCount = 1,
                        FrameType lastFrameType = FRAME_TYPE_P,
                        FrameType preIdrAnchorFrameType = FRAME_TYPE_P);

    bool Init(uint64_t maxNumFrames);

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
    void SetGopFrameCount(uint8_t gopFrameCount) { m_gopFrameCount = gopFrameCount; }
    uint8_t GetGopFrameCount() const { return m_gopFrameCount; }

    // idrPeriod is the interval, in terms of number of frames, between two IDR frames (see IDR period).
    // If it is set to 0, the rate control algorithm may assume an implementation-dependent IDR period.
    // If it is set to UINT8_MAX, the IDR period is treated as infinite.
    void SetIdrPeriod(uint32_t idrPeriod) { m_idrPeriod = idrPeriod; }
    uint32_t GetIdrPeriod() const { return m_idrPeriod; }

    // consecutiveBFrameCount is the number of consecutive B frames between I and/or P frames within the GOP.
    void SetConsecutiveBFrameCount(uint8_t consecutiveBFrameCount) { m_consecutiveBFrameCount = consecutiveBFrameCount; }
    uint8_t GetConsecutiveBFrameCount() const { return m_consecutiveBFrameCount; }

    // specifies the number of H.264/5 sub-layers that the application intends to use.
    void SetTemporalLayerCount(uint8_t temporalLayerCount) { m_temporalLayerCount = temporalLayerCount; }
    uint8_t GetTemporalLayerCount() const { return m_temporalLayerCount; }

    // lastFrameType is the type of frame that will be used for the last frame in the stream.
    // This frame type will replace the type regardless on the type determined by the GOP structure.
    bool SetLastFrameType(FrameType lastFrameType) {

        m_lastFrameType = lastFrameType;
        return true;
    }

    virtual void PrintGopStructure(uint64_t numFrames = uint64_t(-1)) const;

    int32_t GetIdrRefDelta(const GopState& gopState, int32_t& idrPeriodDelta) const
    {
        if (m_idrPeriod > 0) {
            idrPeriodDelta = m_idrPeriod - (gopState.positionInInputOrder % m_idrPeriod);
            const int32_t idrPeriodPositionInInputOrder = idrPeriodDelta + gopState.positionInInputOrder;
            const int32_t refDelta = idrPeriodPositionInInputOrder - gopState.lastRefInInputOrder;
            return refDelta;
        } else {
            idrPeriodDelta = -1;
            return -1;
        }
    }

    // GetPositionInGOP() returns true of it start a new IDR sequence.
    bool GetPositionInGOP(GopState& gopState, GopPosition& gopPos,
                          bool firstFrame = false, bool lastFrame = false) const {

        gopPos = GopPosition(gopState.positionInInputOrder);

        if (firstFrame || ((m_idrPeriod > 0) &&
                ((gopState.positionInInputOrder % m_idrPeriod) == 0))) {

            gopPos.pictureType = FRAME_TYPE_IDR;
            gopPos.inputOrder = 0;  // reset the IDR sequence
            gopPos.flags |= FLAGS_IS_REF | FLAGS_CLOSE_GOP;
            gopState.lastRefInInputOrder  = 0;
            gopState.lastRefInEncodeOrder = 0;
            gopState.positionInInputOrder = 1; // next frame value
            return true;
        }

        gopPos.inputOrder = gopState.positionInInputOrder;

        // consecutiveBFrameCount can be modified before the IDR sequence
        uint8_t consecutiveBFrameCount = m_consecutiveBFrameCount;

        if (lastFrame) {
            // Last frame is always set to the m_lastFrameType
            gopPos.pictureType = m_lastFrameType;
        } else if ((gopState.positionInInputOrder % m_gopFrameCount) == 0) {
            // This is the start of a new (open or close) GOP.
            gopPos.pictureType = FRAME_TYPE_I;
        } else if ((gopState.positionInInputOrder % (consecutiveBFrameCount + 1) == 0)) {
            // This is A P or B frame based on m_consecutiveBFrameCount.
            gopPos.pictureType = FRAME_TYPE_P;
        } else if (consecutiveBFrameCount > 0) {
            // This supposed to be a B frame, if we have a forward anchor
            int32_t idrPeriodDelta = -1;
            const int32_t refDelta = GetIdrRefDelta(gopState, idrPeriodDelta);
            if ((refDelta > 0) && ((consecutiveBFrameCount + 1) >= refDelta)) {
                // This are B frames before the IDR. We can't use B frames only here
                // because we can't use the IDR frame as a forward reference anchor.
                // So we need to introduce one extra I or P ref. frame just before the IDR.

                // consecutiveBFrameCount is the refDelta minus prev ref minus
                // the extra P ref at the end before the IDR
                consecutiveBFrameCount = refDelta - 2;

                if (idrPeriodDelta == 1) { // This is the last frame before the IDR
                    // A promoted B-frame to a reference of type m_preIdrAnchorFrameType
                    gopPos.pictureType = m_preIdrAnchorFrameType;
                } else {
                    // A modified B-frame from the GOP
                    gopPos.pictureType = FRAME_TYPE_B;
                }

            } else {
                // Just a regular B-frame from the GOP
                gopPos.pictureType = FRAME_TYPE_B;
            }
        }

        if (gopPos.pictureType == FRAME_TYPE_B) {
            gopPos.encodeOrder = gopState.positionInInputOrder + 1;
            gopPos.bFramePos = (int8_t)((gopState.positionInInputOrder % (consecutiveBFrameCount + 1)) - 1);
            gopPos.numBFrames = consecutiveBFrameCount;
        } else {

            if (gopState.positionInInputOrder > consecutiveBFrameCount) {
                gopPos.encodeOrder = gopState.positionInInputOrder - consecutiveBFrameCount;
            } else {
                gopPos.encodeOrder = gopState.positionInInputOrder;
            }

            gopPos.flags |= FLAGS_IS_REF;
            gopState.lastRefInInputOrder  = gopState.positionInInputOrder;
            gopState.lastRefInEncodeOrder = gopPos.encodeOrder;
        }

        gopPos.inGop = gopState.positionInInputOrder % m_gopFrameCount;
        gopState.positionInInputOrder++;

        return false;
    }

    bool IsFrameReference(GopPosition& gopPos) const {

        return ((gopPos.flags & FLAGS_IS_REF) != 0);
    }

    virtual void DumpFrameGopStructure(GopState& gopState,
                                       bool firstFrame = false, bool lastFrame = false) const;

    virtual void DumpFramesGopStructure(uint64_t firstFrameNumInInputOrder, uint64_t numFrames) const;

    virtual ~VkVideoGopStructure() {

    }

private:
    uint8_t               m_gopFrameCount;
    uint8_t               m_consecutiveBFrameCount;
    uint8_t               m_gopFrameCycle;
    uint8_t               m_temporalLayerCount;
    uint32_t              m_idrPeriod; // 0 means unlimited GOP with no IDRs.
    FrameType             m_lastFrameType;
    FrameType             m_preIdrAnchorFrameType;
};
#endif /* _VKVIDEOENCODER_VKVIDEOGOPSTRUCTURE_H_ */
