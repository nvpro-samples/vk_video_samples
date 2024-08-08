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

#ifndef _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEH264_H_
#define _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEH264_H_

class VideoSessionParametersInfo {
public:
    VideoSessionParametersInfo(VkVideoSessionKHR videoSession,
                               StdVideoH264SequenceParameterSet* sps,
                               StdVideoH264PictureParameterSet* pps)
    {
        m_videoSession = videoSession;

        m_encodeH264SessionParametersAddInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
        m_encodeH264SessionParametersAddInfo.pNext = nullptr;
        m_encodeH264SessionParametersAddInfo.stdSPSCount = 1;
        m_encodeH264SessionParametersAddInfo.pStdSPSs = sps;
        m_encodeH264SessionParametersAddInfo.stdPPSCount = 1;
        m_encodeH264SessionParametersAddInfo.pStdPPSs = pps;

        m_encodeH264SessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
        m_encodeH264SessionParametersCreateInfo.pNext = nullptr;
        m_encodeH264SessionParametersCreateInfo.maxStdSPSCount = 1;
        m_encodeH264SessionParametersCreateInfo.maxStdPPSCount = 1;
        m_encodeH264SessionParametersCreateInfo.pParametersAddInfo = &m_encodeH264SessionParametersAddInfo;

        m_encodeSessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        m_encodeSessionParametersCreateInfo.pNext = &m_encodeH264SessionParametersCreateInfo;
        m_encodeSessionParametersCreateInfo.videoSessionParametersTemplate = nullptr;
        m_encodeSessionParametersCreateInfo.videoSession = m_videoSession;
    }

    inline VkVideoSessionParametersCreateInfoKHR* getVideoSessionParametersInfo()
    {
        return &m_encodeSessionParametersCreateInfo;
    };
private:
    VkVideoSessionKHR m_videoSession;
    VkVideoEncodeH264SessionParametersAddInfoKHR m_encodeH264SessionParametersAddInfo;
    VkVideoEncodeH264SessionParametersCreateInfoKHR m_encodeH264SessionParametersCreateInfo;
    VkVideoSessionParametersCreateInfoKHR m_encodeSessionParametersCreateInfo;
};

struct EncoderH264State {

    EncoderH264State()
        : m_spsInfo()
        , m_ppsInfo()
        , m_vuiInfo()
        , m_hrdParameters()
        , m_rateControlInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR }
        , m_rateControlLayersInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR }
    {
        m_spsInfo.pSequenceParameterSetVui = &m_vuiInfo;
    }

public:
    StdVideoH264SequenceParameterSet         m_spsInfo;
    StdVideoH264PictureParameterSet          m_ppsInfo;
    StdVideoH264SequenceParameterSetVui      m_vuiInfo;
    StdVideoH264HrdParameters                m_hrdParameters;
    VkVideoEncodeH264RateControlInfoKHR      m_rateControlInfoH264;
    VkVideoEncodeH264RateControlLayerInfoKHR m_rateControlLayersInfoH264[1];
};

#endif /* _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEH264_H_ */
