/*
 * Copyright 2022 NVIDIA Corporation.
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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERH265_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERH265_H_

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderStateH265.h"
#include "VkVideoEncoder/VkEncoderConfigH265.h"
#include "VkVideoEncoder/VkEncoderDpbH265.h"

class VkVideoEncoderH265 : public VkVideoEncoder {

    enum { MAX_REFFERENCES = 16 };
    enum { MAX_NUM_SLICES  = 64 };

    struct VkVideoEncodeFrameInfoH265 : public VkVideoEncodeFrameInfo {

        VkVideoEncodeH265PictureInfoKHR          pictureInfo;
        VkVideoEncodeH265NaluSliceSegmentInfoKHR naluSliceSegmentInfo[MAX_NUM_SLICES];
        StdVideoEncodeH265PictureInfo            stdPictureInfo;
        VkVideoEncodeH265RateControlInfoKHR      rateControlInfoH265;
        VkVideoEncodeH265RateControlLayerInfoKHR rateControlLayersInfoH265[1];
        StdVideoEncodeH265SliceSegmentHeader     stdSliceSegmentHeader[MAX_NUM_SLICES];
        StdVideoEncodeH265ReferenceListsInfo     stdReferenceListsInfo;
        StdVideoH265ShortTermRefPicSet           stdShortTermRefPicSet;
        StdVideoEncodeH265LongTermRefPics        stdLongTermRefPics;
        StdVideoEncodeH265ReferenceInfo          stdReferenceInfo[MAX_REFFERENCES];
        VkVideoEncodeH265DpbSlotInfoKHR          stdDpbSlotInfo[MAX_REFFERENCES];

        VkVideoEncodeFrameInfoH265()
          : VkVideoEncodeFrameInfo(&pictureInfo, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR)
          , pictureInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR }
          , naluSliceSegmentInfo{}
          , stdPictureInfo()
          , rateControlInfoH265{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR }
          , rateControlLayersInfoH265{{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR }}
          , stdSliceSegmentHeader{}
          , stdReferenceListsInfo()
          , stdShortTermRefPicSet()
          , stdLongTermRefPics()
          , stdReferenceInfo{}
          , stdDpbSlotInfo{}
        {
            pictureInfo.naluSliceSegmentEntryCount = 1;
            pictureInfo.pNaluSliceSegmentEntries = naluSliceSegmentInfo;
            pictureInfo.pStdPictureInfo = &stdPictureInfo;

            for (uint32_t i = 0; i < MAX_NUM_SLICES; i++) {
                auto& sliceInfo = naluSliceSegmentInfo[i];

                sliceInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR;
                sliceInfo.pNext = nullptr;
                sliceInfo.pStdSliceSegmentHeader = &stdSliceSegmentHeader[i];
            }

            stdPictureInfo.pRefLists           = &stdReferenceListsInfo;
            stdPictureInfo.pShortTermRefPicSet = &stdShortTermRefPicSet;
            stdPictureInfo.pLongTermRefPics    = &stdLongTermRefPics;

            stdDpbSlotInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
            stdDpbSlotInfo->pStdReferenceInfo = stdReferenceInfo;
        };

        virtual void Reset(bool releaseResources = true) {

            pictureInfo.pNext = nullptr;

            // Reset the base first
            VkVideoEncodeFrameInfo::Reset(releaseResources);

            // After resetting the base structure parameters, start building the pNext chain again
            encodeInfo.pNext = &pictureInfo;

            // Clear and check state
            assert(pictureInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR);
            assert(naluSliceSegmentInfo[0].sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR);
            // stdPictureInfo()
            assert(rateControlInfoH265.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR);
            assert(rateControlLayersInfoH265[0].sType ==  VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR);
            // stdSliceSegmentHeader()
            // stdReferenceListsInfo()
            // stdShortTermRefPicSet()
            // stdLongTermRefPics()
            // stdReferenceInfo{}
            // stdDpbSlotInfo{}
        }

        virtual ~VkVideoEncodeFrameInfoH265() {
            Reset(true);
        }
    };

public:

    VkVideoEncoderH265(const VulkanDeviceContext* vkDevCtx)
        : VkVideoEncoder(vkDevCtx)
        , m_encoderConfig()
        , m_vps{}
        , m_sps{}
        , m_pps{}
        , m_rateControlInfoH265{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR}
        , m_rateControlLayersInfoH265{{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR}}
        , m_dpb{}
    { }

    virtual VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig);
    virtual VkResult InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp);
    virtual VkResult EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                uint32_t frameIdx, uint32_t ofTotalFrames);
    virtual VkResult CreateFrameInfoBuffersQueue(uint32_t numPoolNodes);
    virtual bool GetAvailablePoolNode(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
    {
        VkSharedBaseObj<VkVideoEncodeFrameInfoH265> encodeFrameInfoH265;
        bool success = m_frameInfoBuffersQueue->GetAvailablePoolNode(encodeFrameInfoH265);
        if (success) {
            encodeFrameInfo = encodeFrameInfoH265;
        }
        return success;
    }

protected:
    virtual ~VkVideoEncoderH265() {

        m_frameInfoBuffersQueue = nullptr;
        m_encoderConfig = nullptr;
    }

    // Must be called from VkVideoEncoder::EncodeFrameCommon only
    virtual VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
private:

    VkVideoEncodeFrameInfoH265* GetEncodeFrameInfoH265(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) {
        assert(VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR == encodeFrameInfo->GetType());
        VkVideoEncodeFrameInfo* pEncodeFrameInfo = encodeFrameInfo;
        return (VkVideoEncodeFrameInfoH265*)pEncodeFrameInfo;
    }
private:
    VkSharedBaseObj<EncoderConfigH265>         m_encoderConfig;
    VpsH265                                    m_vps;
    SpsH265                                    m_sps;
    StdVideoH265PictureParameterSet            m_pps;
    VkVideoEncodeH265RateControlInfoKHR        m_rateControlInfoH265;
    VkVideoEncodeH265RateControlLayerInfoKHR   m_rateControlLayersInfoH265[1];
    VkEncDpbH265                               m_dpb;
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoH265>> m_frameInfoBuffersQueue;
};

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERH265_H_ */
