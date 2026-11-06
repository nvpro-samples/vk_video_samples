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

#include <vector>

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkVideoEncoderStateH264.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"
#include "VkVideoEncoder/VkEncoderDpbPictures.h"

class VkVideoEncoderH264 : public VkVideoEncoder {

public:

    enum { NON_VCL_BITSTREAM_OFFSET = 4096 };
    enum { MAX_NUM_SLICES_H264 = 64 };

    struct VkVideoEncodeFrameInfoH264 : public VkVideoEncodeFrameInfo {

        VkVideoEncodeH264PictureInfoKHR          pictureInfo;
        VkVideoEncodeH264NaluSliceInfoKHR        naluSliceInfo[MAX_NUM_SLICES_H264];
        StdVideoEncodeH264PictureInfo            stdPictureInfo;
        StdVideoEncodeH264SliceHeader            stdSliceHeader[MAX_NUM_SLICES_H264];
        VkVideoEncodeH264RateControlInfoKHR      rateControlInfoH264;
        VkVideoEncodeH264RateControlLayerInfoKHR rateControlLayersInfoH264[1];
        // What the DPB library writes for the picture: the reference syntax stdPictureInfo points to,
        // and the slots the video commands bind.
        vvs::dpb::H264EncodeSyntax               dpbSyntax;
        vvs::dpb::PictureResult                  dpbResult;

        VkVideoEncodeFrameInfoH264()
          : VkVideoEncodeFrameInfo(&pictureInfo, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)
          , pictureInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR }
          , naluSliceInfo{}
          , stdPictureInfo()
          , stdSliceHeader{}
          , rateControlInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR }
          , rateControlLayersInfoH264 {{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR }}
          , dpbSyntax()
          , dpbResult()
        {
            pictureInfo.naluSliceEntryCount = 1; // FIXME: support more than one
            pictureInfo.pNaluSliceEntries = naluSliceInfo;
            pictureInfo.pStdPictureInfo = &stdPictureInfo;

            for (uint32_t i = 0; i < MAX_NUM_SLICES_H264; i++) {
                auto& sliceInfo = naluSliceInfo[i];

                sliceInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
                sliceInfo.pNext = nullptr;
                sliceInfo.pStdSliceHeader = &stdSliceHeader[i];
            }
        };

        virtual void Reset(bool releaseResources = true) {
            // Reset the base first
            VkVideoEncodeFrameInfo::Reset(releaseResources);

            // After resetting the base structure parameters, start building the pNext chain again
            pictureInfo.pNext = nullptr;
            encodeInfo.pNext = &pictureInfo;

            // Clear and check state
            assert(pictureInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR);
            assert(naluSliceInfo[0].sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR);
            // stdPictureInfo()
            // stdSliceHeader()
            assert(rateControlInfoH264.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR);
            assert(rateControlLayersInfoH264[0].sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR);
            // dpbSyntax and dpbResult: written whole by each picture's DPB call.
        }

        virtual ~VkVideoEncodeFrameInfoH264() {
            Reset(true);
        }
    };

    VkVideoEncoderH264(const VulkanDeviceContext* vkDevCtx)
        : VkVideoEncoder(vkDevCtx)
        , m_encoderConfig()
        , m_h264()
        , m_dpb()
        , m_dpbPictures()
    { }

    virtual VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig) override;
    virtual VkResult InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp) override;
    // DECLARES rather than overrides -- there is no base-class
    // EncodeVideoSessionParameters -- so it is the one virtual here that
    // correctly carries no 'override', and marking it would not compile.
    virtual VkResult EncodeVideoSessionParameters(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                uint32_t frameIdx,
                                uint32_t ofTotalFrames) override;
    virtual VkResult CreateFrameInfoBuffersQueue(uint32_t numPoolNodes) override;
    virtual bool GetAvailablePoolNode(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) override
    {
        VkSharedBaseObj<VkVideoEncodeFrameInfoH264> encodeFrameInfoH264;
        bool success = m_frameInfoBuffersQueue->GetAvailablePoolNode(encodeFrameInfoH264);
        if (success) {
            encodeFrameInfo = encodeFrameInfoH264;
        }
        return success;
    }

protected:
public:
    virtual ~VkVideoEncoderH264()
    {
        // Must join worker threads before destroying H.264-specific members.
        WaitForThreadsToComplete();

        m_frameInfoBuffersQueue = nullptr;
        m_videoSessionParameters = nullptr;
        m_videoSession = nullptr;
    }

    // Must be called from VkVideoEncoder::EncodeFrameCommon only
    virtual VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) override;
    virtual VkResult CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) override;

    // The H.264 arm of the mid-stream rate-control refresh: the same fill
    // InitEncoderCodec runs once, re-run against the current config so a
    // QP clamp changed by Reconfigure reaches
    // m_h264.m_rateControlLayersInfoH264 -- which is the struct
    // CodecHandleRateControlCmd above chains onto the next command.
    virtual void RefreshCodecRateControlParameters() override;
    virtual void GetResolvedQpClampForTest(uint32_t* pUseMinQp,
                                           int32_t*  pMinQpI,
                                           uint32_t* pUseMaxQp,
                                           int32_t*  pMaxQpI) const override;

private:

    VkVideoEncodeFrameInfoH264* GetEncodeFrameInfoH264(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) {
        assert(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR == encodeFrameInfo->GetType());
        VkVideoEncodeFrameInfo* pEncodeFrameInfo = encodeFrameInfo.get();
        return (VkVideoEncodeFrameInfoH264*)pEncodeFrameInfo;
    }

private:
    VkSharedBaseObj<EncoderConfigH264> m_encoderConfig;
    EncoderH264State                   m_h264;
    vvs::dpb::H264Encode               m_dpb;
    VkEncoderDpbPictures               m_dpbPictures;
    VkSharedBaseObj<VulkanBufferPool<VkVideoEncodeFrameInfoH264>> m_frameInfoBuffersQueue;
};



#endif /* _VKVIDEOENCODER_VKVIDEOENCODERH264_H_ */
