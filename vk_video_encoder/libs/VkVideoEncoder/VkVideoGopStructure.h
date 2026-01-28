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
#include <algorithm>  // for std::min

static const uint32_t MAX_GOP_SIZE = 64;

class VkVideoGopStructure {

public:
    enum FrameType { FRAME_TYPE_P = 0, FRAME_TYPE_B = 1, FRAME_TYPE_I = 2,
                     FRAME_TYPE_IDR = 3, FRAME_TYPE_INTRA_REFRESH = 6, FRAME_TYPE_INVALID = -1 };


    enum Flags { FLAGS_IS_REF         = (1 << 0), // frame is a reference
                 FLAGS_CLOSE_GOP      = (1 << 1), // Last reference in the Gop. Indicates the end of a closed Gop.
                 FLAGS_NONUNIFORM_GOP = (1 << 2), // nonuniform  Gop part of sequence (usually used to terminate Gop).
                 FLAGS_INTRA_REFRESH  = (1 << 3), // This frame is part of an intra-refresh cycle
               };

    struct GopState {
        uint32_t positionInInputOrder;
        uint32_t lastRefInInputOrder;
        uint32_t lastRefInEncodeOrder;
        uint32_t intraRefreshCounter;
        bool     intraRefreshCycleRestarted;
        bool     intraRefreshStartSkipped;

        GopState()
        : positionInInputOrder(0)
        , lastRefInInputOrder(0)
        , lastRefInEncodeOrder(0)
        , intraRefreshCounter(0)
        , intraRefreshCycleRestarted(false)
        , intraRefreshStartSkipped(false) {}
    };

    struct GopPosition {
        uint32_t   inputOrder;  // input order in the IDR sequence
        uint32_t   encodeOrder; // encode order in the IDR sequence
        uint32_t   inGop;       // The position in Gop in input order
        int8_t     numBFrames;  // Number of B frames in this part of the Gop, -1 if not a B frame
        int8_t     bFramePos;   // The B position in Gop, -1 if not a B frame
        FrameType  pictureType;   // The type of the picture
        uint32_t   flags;       // one or multiple of flags of type Flags above
        uint32_t   intraRefreshIndex; // the index of the frame within the intra-refresh cycle

        GopPosition(uint32_t positionInGopInInputOrder)
        : inputOrder(positionInGopInInputOrder)
        , encodeOrder(0)
        , inGop(0)
        , numBFrames(-1)
        , bFramePos(-1)
        , pictureType(FRAME_TYPE_INVALID)
        , flags(0)
        , intraRefreshIndex(UINT32_MAX)
        {}
    };

    VkVideoGopStructure(uint8_t gopFrameCount = 8,
                        int32_t idrPeriod = 60,
                        uint8_t consecutiveBFrameCount = 2,
                        uint8_t temporalLayerCount = 1,
                        FrameType lastFrameType = FRAME_TYPE_P,
                        FrameType preIdrAnchorFrameType = FRAME_TYPE_P,
                        bool m_closedGop = false,
                        uint32_t intraRefreshCycleDuration = 0);

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
    void SetGopFrameCount(uint32_t gopFrameCount) { m_gopFrameCount = gopFrameCount; }
    uint32_t GetGopFrameCount() const { return m_gopFrameCount; }

    // idrPeriod is the interval, in terms of number of frames, between two IDR frames (see IDR period).
    // If it is set to 0, the rate control algorithm may assume an implementation-dependent IDR period.
    // If it is set to UINT8_MAX, the IDR period is treated as infinite.
    void SetIdrPeriod(uint32_t idrPeriod) { m_idrPeriod = idrPeriod; }
    uint32_t GetIdrPeriod() const { return m_idrPeriod; }

    // consecutiveBFrameCount is the number of consecutive B frames between I and/or P frames within the GOP.
    void SetConsecutiveBFrameCount(uint8_t consecutiveBFrameCount) { m_consecutiveBFrameCount = consecutiveBFrameCount; }
    uint8_t GetConsecutiveBFrameCount() const { return m_consecutiveBFrameCount; }

    void SetIntraRefreshCycleDuration(uint32_t intraRefreshCycleDuration) { m_intraRefreshCycleDuration = intraRefreshCycleDuration; }

    void SetIntraRefreshCycleRestartIndex(uint32_t intraRefreshCycleRestartIndex) { m_intraRefreshCycleRestartIndex = intraRefreshCycleRestartIndex; }

    void SetIntraRefreshSkippedStartIndex(uint32_t intraRefreshSkippedStartIndex) { m_intraRefreshSkippedStartIndex = intraRefreshSkippedStartIndex; }

    // specifies the number of H.264/5 sub-layers that the application intends to use.
    void SetTemporalLayerCount(uint8_t temporalLayerCount) { m_temporalLayerCount = temporalLayerCount; }
    uint8_t GetTemporalLayerCount() const { return m_temporalLayerCount; }

    void SetClosedGop() { m_closedGop = true; }
    bool IsClosedGop() { return m_closedGop; }

    // lastFrameType is the type of frame that will be used for the last frame in the stream.
    // This frame type will replace the type regardless on the type determined by the GOP structure.
    bool SetLastFrameType(FrameType lastFrameType) {

        m_lastFrameType = lastFrameType;
        return true;
    }

    virtual void PrintGopStructure(uint64_t numFrames = uint64_t(-1)) const;

    uint32_t GetPeriodDelta(const GopState& gopState, uint32_t period) const
    {
        if (period > 0) {
            return (period - (gopState.positionInInputOrder % period));
        } else {
            return INT32_MAX;
        }
    }

    uint32_t GetRefDelta(const GopState& gopState, uint32_t periodDelta) const
    {
        const uint32_t periodPositionInInputOrder = periodDelta + gopState.positionInInputOrder;
        return periodPositionInInputOrder - gopState.lastRefInInputOrder;
    }

    // GetPositionInGOP() returns true of it start a new IDR sequence.
    bool GetPositionInGOP(GopState& gopState, GopPosition& gopPos,
                          bool firstFrame = false, uint32_t framesLeft = uint32_t(-1)) const {

        gopPos = GopPosition(gopState.positionInInputOrder);

        if (firstFrame || ((m_idrPeriod > 0) &&
                ((gopState.positionInInputOrder % m_idrPeriod) == 0))) {

            gopPos.pictureType = FRAME_TYPE_IDR;
            gopPos.inputOrder = 0;  // reset the IDR sequence
            gopPos.flags |= FLAGS_IS_REF;
            gopState.lastRefInInputOrder  = 0;
            gopState.lastRefInEncodeOrder = 0;
            gopState.positionInInputOrder = 1U; // next frame value
            gopState.intraRefreshCounter = 0;
            return true;
        }

        gopPos.inputOrder = gopState.positionInInputOrder;

        // consecutiveBFrameCount variable is defined as follows:
        // 1. For I or P frames: it equals the number of consecutive B‑frames the immediately precede this frame.
        // 2. For B frames: it represents the size of the consecutive B‑frame group that contains this frame.
        //
        // consecutiveBFrameCount may be adjusted under the following conditions:
        // 1. Closed GOP: lastRefInInputOrder + m_consecutiveBFrameCount + 1 exceeds m_gopFrameCount
        // 2. Open GOP: lastRefInInputOrder + m_consecutiveBFrameCount exceeds m_gopFrameCount
        // 3. IDR period limit: lastRefInInputOrder + m_consecutiveBFrameCount + 1 exceeds idrPeriod
        // 4. Sequence boundary: lastRefInInputOrder + m_consecutiveBFrameCount + 1 exceeds the total
        //      number of frames available in the sequence.
        // NOTE: The “+ 1” in the conditions above accounts for a promoted B picture.
        uint8_t consecutiveBFrameCount = m_consecutiveBFrameCount;

        gopPos.inGop = gopState.positionInInputOrder % m_gopFrameCount;

        if (gopPos.inGop == 0) {
            // This is the start of a new (open or close) GOP.
            gopPos.pictureType = FRAME_TYPE_I;
            consecutiveBFrameCount = gopState.positionInInputOrder - gopState.lastRefInInputOrder - 1;
            gopState.intraRefreshCounter = 0;
        } else if ((gopPos.inGop % m_gopFrameCycle) == 0) {
            // start of min/sub-GOP
            gopPos.pictureType = FRAME_TYPE_P;
            consecutiveBFrameCount = gopState.positionInInputOrder - gopState.lastRefInInputOrder - 1;
        } else if (consecutiveBFrameCount > 0) {
            // This supposed to be a B frame, if we have a forward anchor

            // A Bframe is upgraded to a P frame under the following conditions:
            // 1. It is the final frame in the entire sequence.
            // 2. It is the final frame within the current IDR period.
            // 3. It is the final frame in a closed GOP.
            if ((framesLeft == 1) ||
                ((m_idrPeriod > 0) && (gopState.positionInInputOrder == m_idrPeriod - 1)) ||
                (m_closedGop && (gopPos.inGop == m_gopFrameCount - 1))) {

                gopPos.pictureType = m_preClosedGopAnchorFrameType;
                gopPos.flags |= FLAGS_CLOSE_GOP;
                consecutiveBFrameCount = gopState.positionInInputOrder - gopState.lastRefInInputOrder - 1;
            } else {
                // This is a B picture
                gopPos.pictureType = FRAME_TYPE_B;
                gopPos.bFramePos = gopState.positionInInputOrder - gopState.lastRefInInputOrder - 1;

                // consecutiveBFrameCount is computed as the sum of this frame’s distance to:
                //   - the previous reference frame (lastRefFrame), and
                //   - the next reference frame (nextRefFrame).
                //
                // The distance to the previous reference frame is given by gopPos.bFramePos.
                //
                // To determine the distance to the next reference frame, we must locate nextRefFrame.
                // The nextRefFrame is the nearest frame among the following candidates:
                //   1. The final frame of the entire sequence.
                //   2. The final frame of the current IDR period.
                //   3. The final frame of the closed GOP.
                //   4. The first frame of the min/sub-GOP.
                uint32_t nextRefDelta = framesLeft - 1;
                if (m_idrPeriod > 0) {
                    nextRefDelta = std::min<uint32_t>(nextRefDelta, GetPeriodDelta(gopState, m_idrPeriod) - 1);
                }
                nextRefDelta = std::min<uint32_t>(nextRefDelta, (GetPeriodDelta(gopState, m_gopFrameCount) - (m_closedGop ? 1 : 0)));
                nextRefDelta = std::min<uint32_t>(nextRefDelta, gopState.lastRefInInputOrder + m_gopFrameCycle - gopPos.inputOrder);

                consecutiveBFrameCount = gopPos.bFramePos + nextRefDelta;
                gopPos.numBFrames = consecutiveBFrameCount;
            }
        }

        if (gopPos.pictureType == FRAME_TYPE_B) {
            gopPos.encodeOrder = gopState.positionInInputOrder + 1U;
        } else {

            if (gopState.positionInInputOrder > consecutiveBFrameCount) {
                gopPos.encodeOrder = gopState.positionInInputOrder - consecutiveBFrameCount;
            } else {
                gopPos.encodeOrder = gopState.positionInInputOrder;
            }

            gopPos.flags |= FLAGS_IS_REF;

            // Edge case fix: When a P-frame naturally falls on the position just before
            // an IDR boundary (based on the B-frame pattern, not promoted from B-frame),
            // it should still get FLAGS_CLOSE_GOP. This handles the case where GOP period
            // aligns with IDR period (e.g., GOP=10, IDR=10).
            if ((m_idrPeriod > 0) && ((gopState.positionInInputOrder + 1) % m_idrPeriod == 0)) {
                gopPos.flags |= FLAGS_CLOSE_GOP;
            } else if (m_closedGop && (gopPos.inGop == (m_gopFrameCount - 1))) {
                gopPos.flags |= FLAGS_CLOSE_GOP;
            }

            gopState.lastRefInInputOrder  = gopState.positionInInputOrder;
            gopState.lastRefInEncodeOrder = gopPos.encodeOrder;
        }

        if ((gopPos.pictureType == FRAME_TYPE_P || gopPos.pictureType == FRAME_TYPE_B) &&
            (m_intraRefreshCycleDuration > 0)) {

            // Check if the intra-refresh cycle needs to be restarted. This is useful
            // only for testing that an existing intra-refresh cycle can be
            // interrupted to start a new intra-refresh cycle (also called as
            // "mid-way intra-refresh").
            //
            // m_intraRefreshCycleRestartIndex == 0 is a no-op and is set when mid-way
            // intra-refresh was not requested. If mid-way intra-refresh was
            // requested, the option parsing logic ensures that
            // 1 <= m_intraRefreshCycleRestartIndex < intraRefreshCycleDuration .
            if (!gopState.intraRefreshCycleRestarted &&
                (gopState.intraRefreshCounter >= m_intraRefreshCycleRestartIndex)) {

                gopState.intraRefreshCounter = 0;
                gopState.intraRefreshCycleRestarted = true;
            }

            // Check if the intra-refresh cycle needs a "skipped start" i.e., a start
            // with an intra-refresh index > 0. This is to be used only for testing
            // purposes, to check that the intra-refresh implementation is stateless.
            //
            // m_intraRefreshSkippedStartIndex == 0 is a no-op and is set when
            // intra-refresh with a skipped start was not requested. If intra-refresh
            // with a skipped start was requested, the option parsing logic ensures
            // that 1 <= m_intraRefreshSkippedStartIndex < intraRefreshCycleDuration .
            if (gopState.intraRefreshCounter == 0) {
                if (!gopState.intraRefreshStartSkipped) {
                    // A new intra-refresh cycle needs to be started but with a
                    // skipped start.
                    gopState.intraRefreshCounter = m_intraRefreshSkippedStartIndex;
                    gopState.intraRefreshStartSkipped = true;
                } else {
                    // The previous intra-refresh cycle had a skipped start. Make
                    // the current intra-refresh cycle a full cycle.
                    gopState.intraRefreshStartSkipped = false;
                }
            }

            gopPos.intraRefreshIndex = gopState.intraRefreshCounter;
            gopPos.flags |= FLAGS_INTRA_REFRESH;

            gopState.intraRefreshCounter = (gopState.intraRefreshCounter + 1) % m_intraRefreshCycleDuration;

            // A full intra-refresh cycle has completed and a new intra-refresh cycle
            // will begin at the next frame. Allow a mid-way restart of intra-refresh.
            if (gopState.intraRefreshCounter == 0) {
                gopState.intraRefreshCycleRestarted = false;
            }
        }

        gopState.positionInInputOrder++;

        return false;
    }

    bool IsFrameReference(GopPosition& gopPos) const {

        return ((gopPos.flags & FLAGS_IS_REF) != 0);
    }

    bool IsIntraRefreshFrame(GopPosition& gopPos) const {

        return ((gopPos.flags & FLAGS_INTRA_REFRESH) != 0);
    }

    virtual void DumpFrameGopStructure(GopState& gopState,
                                       bool firstFrame = false, bool lastFrame = false) const;

    virtual void DumpFramesGopStructure(uint64_t firstFrameNumInInputOrder, uint64_t numFrames) const;

    virtual ~VkVideoGopStructure() {

    }

private:
    uint32_t              m_gopFrameCount;
    uint8_t               m_consecutiveBFrameCount;
    uint8_t               m_gopFrameCycle;
    uint8_t               m_temporalLayerCount;
    uint32_t              m_idrPeriod; // 0 means unlimited GOP with no IDRs.
    FrameType             m_lastFrameType;
    FrameType             m_preClosedGopAnchorFrameType;
    uint32_t              m_closedGop : 1;
    uint32_t              m_intraRefreshCycleDuration;
    uint32_t              m_intraRefreshCycleRestartIndex;
    uint32_t              m_intraRefreshSkippedStartIndex;
};
#endif /* _VKVIDEOENCODER_VKVIDEOGOPSTRUCTURE_H_ */
