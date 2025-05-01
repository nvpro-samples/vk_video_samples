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
                               StdVideoH264PictureParameterSet* pps,
                               uint32_t qualityLevel,
                               bool enableQpMap = false, VkExtent2D qpMapTexelSize = {0, 0})
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
        m_encodeSessionParametersCreateInfo.flags = 0;
        m_encodeSessionParametersCreateInfo.videoSessionParametersTemplate = VK_NULL_HANDLE;
        m_encodeSessionParametersCreateInfo.videoSession = m_videoSession;

        m_qualityLevelInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
        m_qualityLevelInfo.pNext = nullptr;
        m_qualityLevelInfo.qualityLevel = qualityLevel;

        m_encodeH264SessionParametersCreateInfo.pNext = &m_qualityLevelInfo;

        if (enableQpMap) {
            m_encodeQuantizationMapSessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_SESSION_PARAMETERS_CREATE_INFO_KHR;
            m_encodeQuantizationMapSessionParametersCreateInfo.pNext = nullptr;
            m_encodeQuantizationMapSessionParametersCreateInfo.quantizationMapTexelSize = qpMapTexelSize;

            m_qualityLevelInfo.pNext = &m_encodeQuantizationMapSessionParametersCreateInfo;

            m_encodeSessionParametersCreateInfo.flags = VK_VIDEO_SESSION_PARAMETERS_CREATE_QUANTIZATION_MAP_COMPATIBLE_BIT_KHR;
        }
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
    VkVideoEncodeQualityLevelInfoKHR m_qualityLevelInfo;
    VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR m_encodeQuantizationMapSessionParametersCreateInfo;
};

struct EncoderH264State {

    EncoderH264State()
        : m_spsInfo()
        , m_ppsInfo()
        , m_vuiInfo()
        , m_hrdParameters()
        , m_rateControlInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR }
        , m_rateControlLayersInfoH264{{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR }}
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
