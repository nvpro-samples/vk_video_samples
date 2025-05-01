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

#ifndef _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEAV1_H_
#define _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEAV1_H_

class VideoSessionParametersInfoAV1 {
public:
    VideoSessionParametersInfoAV1(VkVideoSessionKHR videoSession,
                               StdVideoAV1SequenceHeader* seqHdr,
                               StdVideoEncodeAV1DecoderModelInfo* decoderModel,
                               uint32_t operatingPointsCnt,
                               StdVideoEncodeAV1OperatingPointInfo* opInfo,
                               uint32_t qualityLevel,
                               bool enableQpMap, VkExtent2D quantizationMapTexelSize)
    {
        m_videoSession = videoSession;

        m_encodeAV1SessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR;
        m_encodeAV1SessionParametersCreateInfo.pNext = nullptr;
        m_encodeAV1SessionParametersCreateInfo.pStdSequenceHeader = seqHdr;
        m_encodeAV1SessionParametersCreateInfo.pStdDecoderModelInfo = decoderModel;
        m_encodeAV1SessionParametersCreateInfo.stdOperatingPointCount = operatingPointsCnt;
        m_encodeAV1SessionParametersCreateInfo.pStdOperatingPoints = opInfo;

        m_sessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        m_sessionParametersCreateInfo.pNext = &m_encodeAV1SessionParametersCreateInfo;
        m_sessionParametersCreateInfo.flags = 0;
        m_sessionParametersCreateInfo.videoSessionParametersTemplate = VK_NULL_HANDLE;
        m_sessionParametersCreateInfo.videoSession = m_videoSession;

        m_qualityLevelInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
        m_qualityLevelInfo.pNext = nullptr;
        m_qualityLevelInfo.qualityLevel = qualityLevel;

        m_encodeAV1SessionParametersCreateInfo.pNext = &m_qualityLevelInfo;

        if (enableQpMap) {
            m_quantizationMapSessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_SESSION_PARAMETERS_CREATE_INFO_KHR;
            m_quantizationMapSessionParametersCreateInfo.pNext = nullptr;
            m_quantizationMapSessionParametersCreateInfo.quantizationMapTexelSize = quantizationMapTexelSize;

            m_qualityLevelInfo.pNext = &m_quantizationMapSessionParametersCreateInfo;

            m_sessionParametersCreateInfo.flags = VK_VIDEO_SESSION_PARAMETERS_CREATE_QUANTIZATION_MAP_COMPATIBLE_BIT_KHR;
        }
    }

    inline VkVideoSessionParametersCreateInfoKHR* getVideoSessionParametersInfo()
    {
        return &m_sessionParametersCreateInfo;
    }


private:
    VkVideoSessionKHR m_videoSession;
    VkVideoEncodeAV1SessionParametersCreateInfoKHR m_encodeAV1SessionParametersCreateInfo;
    VkVideoEncodeQualityLevelInfoKHR m_qualityLevelInfo;
    VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR m_quantizationMapSessionParametersCreateInfo;
    VkVideoSessionParametersCreateInfoKHR m_sessionParametersCreateInfo;
};

struct EncoderAV1State {

    EncoderAV1State()
        : m_sequenceHeader()
        , m_timingInfo()
        , m_decoderModelInfo()
        , m_operatingPointsCount()
        , m_operatingPointsInfo()
        , m_rateControlInfoAV1{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR }
        , m_rateControlLayersInfoAV1{{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR }}
        , m_timing_info_present_flag()
        , m_decoder_model_info_present_flag()
    {
    }

public:
    StdVideoAV1SequenceHeader               m_sequenceHeader;
    StdVideoAV1TimingInfo                   m_timingInfo;
    StdVideoEncodeAV1DecoderModelInfo       m_decoderModelInfo;
    uint32_t                                m_operatingPointsCount;
    StdVideoEncodeAV1OperatingPointInfo     m_operatingPointsInfo[32];
    VkVideoEncodeAV1RateControlInfoKHR      m_rateControlInfoAV1;
    VkVideoEncodeAV1RateControlLayerInfoKHR m_rateControlLayersInfoAV1[1];

    bool m_timing_info_present_flag;
    bool m_decoder_model_info_present_flag;
};

#endif /* _LIBS_VKVIDEOENCODER_VKVIDEOENCODERSTATEAV1_H_ */
