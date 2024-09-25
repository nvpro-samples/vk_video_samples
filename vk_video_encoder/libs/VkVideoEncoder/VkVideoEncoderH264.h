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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERH264_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERH264_H_

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderStateH264.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"

class VkVideoEncoderH264 : public VkVideoEncoder {

public:

    enum { NON_VCL_BITSTREAM_OFFSET = 4096 };
    enum { MAX_NUM_SLICES_H264 = 64 };
    enum { MAX_MEM_MGMNT_CTRL_OPS_COMMANDS = 16 }; // max mmco commands.
    enum { MAX_REFFERENCES = 16 };

    struct VkVideoEncodeFrameInfoH264 : public VkVideoEncodeFrameInfo {

        VkVideoEncodeH264PictureInfoKHR          pictureInfo;
        VkVideoEncodeH264NaluSliceInfoKHR        naluSliceInfo;
        StdVideoEncodeH264PictureInfo            stdPictureInfo;
        StdVideoEncodeH264SliceHeader            stdSliceHeader;
        VkVideoEncodeH264RateControlInfoKHR      rateControlInfoH264;
        VkVideoEncodeH264RateControlLayerInfoKHR rateControlLayersInfoH264[1];
        StdVideoEncodeH264ReferenceListsInfo     stdReferenceListsInfo;
        StdVideoEncodeH264ReferenceInfo          stdReferenceInfo[MAX_REFFERENCES];
        VkVideoEncodeH264DpbSlotInfoKHR          stdDpbSlotInfo[MAX_REFFERENCES];
        StdVideoEncodeH264RefListModEntry        refList0ModOperations[MAX_REFFERENCES];
        StdVideoEncodeH264RefListModEntry        refList1ModOperations[MAX_REFFERENCES];
        StdVideoEncodeH264RefPicMarkingEntry     refPicMarkingEntry[MAX_MEM_MGMNT_CTRL_OPS_COMMANDS];

        VkVideoEncodeFrameInfoH264()
          : VkVideoEncodeFrameInfo(&pictureInfo)
          , pictureInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR }
          , naluSliceInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR }
          , stdPictureInfo()
          , stdSliceHeader()
          , rateControlInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR }
          , rateControlLayersInfoH264 {{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR }}
          , stdReferenceListsInfo()
          , stdReferenceInfo{}
          , stdDpbSlotInfo{}
          , refList0ModOperations{}
          , refList1ModOperations{}
          , refPicMarkingEntry{}
        {
            pictureInfo.naluSliceEntryCount = 1; // FIXME: support more than one
            pictureInfo.pNaluSliceEntries = &naluSliceInfo;
            pictureInfo.pStdPictureInfo = &stdPictureInfo;
            naluSliceInfo.pStdSliceHeader = &stdSliceHeader;

            stdPictureInfo.pRefLists           = &stdReferenceListsInfo;

            stdDpbSlotInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
            stdDpbSlotInfo->pStdReferenceInfo = stdReferenceInfo;
        };

        virtual void Reset(bool releaseResources = true) {

            pictureInfo.pNext = nullptr;

            // Reset the base first
            VkVideoEncodeFrameInfo::Reset(releaseResources);

            // Clear and check state
            assert(pictureInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR);
            assert(naluSliceInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR);
            // stdPictureInfo()
            // stdSliceHeader()
            assert(rateControlInfoH264.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR);
            assert(rateControlLayersInfoH264[0].sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR);
            // stdReferenceListsInfo()
            // stdReferenceInfo{}
            // stdDpbSlotInfo{}
            // refList0ModOperations{}
            // refList1ModOperations{}
            // refPicMarkingEntry{}
        }

        virtual ~VkVideoEncodeFrameInfoH264() {
            Reset(true);
        }
    };

    VkVideoEncoderH264(const VulkanDeviceContext* vkDevCtx)
        : VkVideoEncoder(vkDevCtx)
        , m_encoderConfig()
        , m_h264()
        , m_dpb264()
    { }

    virtual VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig);
    virtual VkResult InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp);
    virtual VkResult EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                uint32_t frameIdx, uint32_t ofTotalFrames);
    virtual VkResult CreateFrameInfoBuffersQueue(uint32_t numPoolNodes);
    virtual bool GetAvailablePoolNode(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo)
    {
        VkSharedBaseObj<VkVideoEncodeFrameInfoH264> encodeFrameInfoH264;
        bool success = m_frameInfoBuffersQueue->GetAvailablePoolNode(encodeFrameInfoH264);
        if (success) {
            encodeFrameInfo = encodeFrameInfoH264;
        }
        return success;
    }

    virtual VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);

protected:
    virtual ~VkVideoEncoderH264()
    {

        m_frameInfoBuffersQueue = nullptr;
        m_videoSessionParameters = nullptr;
        m_videoSession = nullptr;

        if (m_dpb264) {
            m_dpb264->DpbDestroy();
            m_dpb264 = NULL;
        }
    }

private:

    VkVideoEncodeFrameInfoH264* GetEncodeFrameInfoH264(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) {
        assert(VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR == encodeFrameInfo->GetType());
        VkVideoEncodeFrameInfo* pEncodeFrameInfo = encodeFrameInfo;
        return (VkVideoEncodeFrameInfoH264*)pEncodeFrameInfo;
    }

    void POCBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                  uint8_t& m_refPicMarkingOpCount);

    void FrameNumBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                       uint8_t& m_refPicMarkingOpCount);

    VkResult SetupRefPicReorderingCommands(const PicInfoH264 *pPicInfo,
                                           const StdVideoEncodeH264SliceHeader *slh,
                                           StdVideoEncodeH264ReferenceListsInfoFlags* pFlags,
                                           StdVideoEncodeH264RefListModEntry* m_ref_pic_list_modification_l0,
                                           uint8_t& m_refList0ModOpCount);

private:
    VkSharedBaseObj<EncoderConfigH264> m_encoderConfig;
    EncoderH264State                   m_h264;
    VkEncDpbH264*                      m_dpb264;
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoH264>> m_frameInfoBuffersQueue;
};



#endif /* _VKVIDEOENCODER_VKVIDEOENCODERH264_H_ */
