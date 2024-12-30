/*
* Copyright 2023 NVIDIA Corporation.
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

#ifndef _VP9_PROBMANAGER_H_
#define _VP9_PROBMANAGER_H_

#include <stdint.h>
#include <climits>

#include "VulkanVideoDecoder.h"

#define VP9_FRAME_MARKER 2
#define VP9_FRAME_SYNC_CODE 0x498342
#define VP9_MAX_PRBABILITY 255
#define VP9_MIN_TILE_WIDTH_B64 4
#define VP9_MAX_TILE_WIDTH_B64 64
#define ROUND_POWER_OF_TWO(value, n) (((value) + (1 << ((n) - 1))) >> (n))
#define ALIGN_POWER_OF_TWO(value, n) (((value) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))

#define VP9_BUFFER_POOL_MAX_SIZE 10
#define VP9_MAX_NUM_SPATIAL_LAYERS 4

#define VP9_CHECK_FRAME_MARKER {    \
  if (u(2) != VP9_FRAME_MARKER) {   \
    assert(!"Invalid frame marker");\
    return false;                   \
  }                                 \
}

#define VP9_CHECK_ZERO_BIT {    \
  if (u(1) != 0) {              \
    assert("!Invalid syntax");  \
    return false;               \
  }                             \
}

#define VP9_CHECK_FRAME_SYNC_CODE   {   \
  if (u(24) != VP9_FRAME_SYNC_CODE) {   \
    assert("!Invalid frame sync code"); \
  }                                     \
}

// Segment level features.
typedef enum {
  SEG_LVL_ALT_Q = 0,               // Use alternate Quantizer ....
  SEG_LVL_ALT_LF = 1,              // Use alternate loop filter value...
  SEG_LVL_REF_FRAME = 2,           // Optional Segment reference frame
  SEG_LVL_SKIP = 3,                // Optional Segment (0,0) + skip mode
  SEG_LVL_MAX = 4                  // Number of MB level features supported
} SEG_LVL_FEATURES;

typedef struct _vp9_ref_frames_s {
    VkPicIf* buffer;
    StdVideoVP9FrameType frame_type;
    bool segmentation_enabled;
} vp9_ref_frames_s;

class VulkanVP9Decoder : public VulkanVideoDecoder
{
protected:
    VkParserVp9PictureData m_PicData;

    VkPicIf*      m_pCurrPic;
    VkPicIf*      m_pOutFrame[VP9_MAX_NUM_SPATIAL_LAYERS];

    int           m_frameIdx;
    int           m_dataSize;
    int           m_frameSize;
    bool          m_frameSizeChanged;

    int           m_rtOrigWidth;
    int           m_rtOrigHeight;
    bool          m_pictureStarted;
    bool          m_bitstreamComplete;

    // Parsing state for compute_image_size() side effects
    int           m_lastFrameWidth;
    int           m_lastFrameHeight;
    bool          m_lastShowFrame;

    // Last used loop filter parameters
    int8_t        m_loopFilterRefDeltas[STD_VIDEO_VP9_MAX_REF_FRAMES];
    int8_t        m_loopFilterModeDeltas[STD_VIDEO_VP9_LOOP_FILTER_ADJUSTMENTS];

    vp9_ref_frames_s m_pBuffers[VP9_BUFFER_POOL_MAX_SIZE];
 
protected:
    void UpdateFramePointers(VkPicIf* currentPicture);
    bool AddBuffertoOutputQueue(VkPicIf* pDispPic);
    void AddBuffertoDispQueue(VkPicIf* pDispPic);
    virtual void lEndPicture(VkPicIf* pDispPic);
    void EndOfStream() override;

public:
    VulkanVP9Decoder(VkVideoCodecOperationFlagBitsKHR std);
    ~VulkanVP9Decoder();

    // TODO: Need to implement these functions.
    bool                    IsPictureBoundary(int32_t) override { return true; };
    int32_t                 ParseNalUnit() override { return NALU_UNKNOWN; };
    bool                    DecodePicture(VkParserPictureData *) { return false; };
    void                    InitParser() override;
    bool                    BeginPicture(VkParserPictureData *) override;
    void                    CreatePrivateContext() override {}
    void                    FreeContext() override {}

private:
    bool                    ParseByteStream(const VkParserBitstreamPacket* pck, size_t* pParsedBtes) override;
    bool                    ParseFrameHeader(uint32_t framesize);
    bool                    ParseUncompressedHeader();
    bool                    ParseColorConfig();
    void                    ParseFrameAndRenderSize();
    void                    ParseFrameAndRenderSizeWithRefs();
    void                    ComputeImageSize();
    void                    ParseLoopFilterParams();
    void                    ParseQuantizationParams();
    int32_t                 ReadDeltaQ();
    void                    ParseSegmentationParams();
    uint8_t                 CalcMinLog2TileCols();
    uint8_t                 CalcMaxLog2TileCols();
    void                    ParseTileInfo();
    void                    ParseSuperFrameIndex(const uint8_t* data, uint32_t data_sz, uint32_t sizes[8], uint32_t* count);

};

#endif // _VP9_PROBMANAGER_H_
