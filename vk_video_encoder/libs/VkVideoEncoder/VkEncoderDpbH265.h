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

#if !defined(NVENC_HEVC_DPB_H)
#define NVENC_HEVC_DPB_H

#include <stdint.h>
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_h265std_encode.h"
#include "VkCodecUtils/VulkanVideoImagePool.h"

struct DpbEntryH265 {
    uint32_t  state : 1;   // 0: empty, 1: in use
    uint32_t  marking : 2; // 0: unused, 1: short-term, 2: long-term
    uint32_t  output : 1;  // 0: not needed for output, 1: needed for output
    uint32_t  corrupted : 1;
    uint32_t picOrderCntVal;
    int32_t  refPicOrderCnt[STD_VIDEO_H265_MAX_DPB_SIZE];
    uint32_t longTermRefPic; // reference marked as long term reference bitfield array with size of the refPicOrderCnt

    // The YCbCr dpb image resource
    VkSharedBaseObj<VulkanVideoImagePoolNode>  dpbImageView;
    uint64_t frameId;      // internal unique id
    int32_t  temporalId;
};

class VkEncDpbH265 {

public:
    struct RefPicSet {
        int8_t                        stCurrBefore[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        int8_t                        stCurrAfter[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        int8_t                        ltCurr[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        int8_t                        stFoll[STD_VIDEO_H265_MAX_NUM_LIST_REF];
        int8_t                        ltFoll[STD_VIDEO_H265_MAX_NUM_LIST_REF];

#if 0
        RefPicSet() {
            // set all entries to "no reference picture"
            memset(this, 0, sizeof(RefPicSet));
        }
#endif
    };

    VkEncDpbH265();
    ~VkEncDpbH265() {}

    bool DpbSequenceStart(int32_t dpbSize, bool useMultipleReferences);

    void ReferencePictureMarking(int32_t curPOC, StdVideoH265PictureType picType,
                                 bool longTermRefPicsPresentFlag);
    void InitializeRPS(const StdVideoH265ShortTermRefPicSet *pSpsShortTermRps,
                       uint8_t spsNumShortTermRefPicSets,
                       StdVideoEncodeH265PictureInfo *pPicInfo,
                       StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                       uint32_t numRefL0, uint32_t numRefL1);
    int8_t DpbPictureStart(uint64_t frameId, const StdVideoEncodeH265PictureInfo *pPicInfo,
                           const StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                           const StdVideoH265LongTermRefPicsSps* pLongTermRefPicsSps,
                           int32_t maxPicOrderCntLsb,
                           uint64_t timeStamp,
                           RefPicSet* pRefPicSet);
    void SetupReferencePictureListLx(StdVideoH265PictureType picType,
                                     const RefPicSet* pRefPicSet,
                                     StdVideoEncodeH265ReferenceListsInfo *pRefLists,
                                     uint32_t numRefL0, uint32_t numRefL1);
    void DpbPictureEnd(VkSharedBaseObj<VulkanVideoImagePoolNode>&  dpbImageView, uint32_t numTemporalLayers, bool isReference);

    bool GetRefPicture(int8_t dpbIndex, VkSharedBaseObj<VulkanVideoImagePoolNode>& dpbImageView);

    void FillStdReferenceInfo(uint8_t dpbIndex, StdVideoEncodeH265ReferenceInfo *pRefInfo);

    void ReferencePictureListIntializationLx(int32_t refPicListLx[2][STD_VIDEO_H265_MAX_NUM_LIST_REF], int32_t refPicListSize[2], const StdVideoEncodeH265SliceSegmentHeader *slh);

private:
    void FlushDpb();
    void DpbBumping();
    bool IsDpbEmpty();
    bool IsDpbFull();

    void ApplyReferencePictureSet(const StdVideoEncodeH265PictureInfo *pPicInfo,
                                  const StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                  const StdVideoH265LongTermRefPicsSps* pLongTermRefPicsSps,
                                  int32_t maxPicOrderCntLsb,
                                  RefPicSet* pRefPicSet);
    // void InitializeLongTermRPSPFrame(int32_t &numPocLtCurr, StdVideoEncodeH265SliceSegmentHeader *slh);
    void InitializeShortTermRPSPFrame(int32_t numPocLtCurr,
                                      const StdVideoH265ShortTermRefPicSet *pSpsShortTermRps,
                                      uint8_t spsNumShortTermRefPicSets,
                                      StdVideoEncodeH265PictureInfo *pPicInfo,
                                      StdVideoH265ShortTermRefPicSet *pShortTermRefPicSet,
                                      uint32_t numRefL0, uint32_t numRefL1);

private:
    DpbEntryH265                   m_stDpb[STD_VIDEO_H265_MAX_DPB_SIZE];
    int8_t                         m_curDpbIndex;
    int8_t                         m_dpbSize;

    int8_t                         m_numPocStCurrBefore;
    int8_t                         m_numPocStCurrAfter;
    int8_t                         m_numPocStFoll;
    int8_t                         m_numPocLtCurr;
    int8_t                         m_numPocLtFoll;

    uint64_t                       m_lastIDRTimeStamp;
    int32_t                        m_picOrderCntCRA;
    bool                           m_refreshPending;
    uint32_t                       m_longTermFlags;
    bool                           m_useMultipleRefs;
};

#endif // !defined(NVENC_HEVC_DPB_H)
