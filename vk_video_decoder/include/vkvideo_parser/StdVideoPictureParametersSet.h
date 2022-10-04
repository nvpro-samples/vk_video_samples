/*
* Copyright 2021 NVIDIA Corporation.
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

#ifndef _NVVKDECODER_STDVIDEOPICTUREPARAMETERSSET_H_
#define _NVVKDECODER_STDVIDEOPICTUREPARAMETERSSET_H_

struct SpsVideoH264PictureParametersSet
{
    StdVideoH264SequenceParameterSet    stdSps;
    int32_t                             offset_for_ref_frame[255];
    StdVideoH264SequenceParameterSetVui stdVui;
    StdVideoH264HrdParameters           stdHrdParameters;
    StdVideoH264ScalingLists            spsStdScalingLists;
};

struct PpsVideoH264PictureParametersSet
{
    StdVideoH264PictureParameterSet     stdPps;
    StdVideoH264ScalingLists            ppsStdScalingLists;
};

struct SpsVideoH265VideoParametersSet
{
    StdVideoH265VideoParameterSet       stdVps;
    StdVideoH265DecPicBufMgr            stdDecPicBufMgr;
    StdVideoH265ProfileTierLevel        stdProfileTierLevel;
};

struct SpsVideoH265PictureParametersSet
{
    StdVideoH265SequenceParameterSet    stdSps;
    StdVideoH265SequenceParameterSetVui stdVui;
    StdVideoH265ScalingLists            spsStdScalingLists;
};

struct PpsVideoH265PictureParametersSet
{
    StdVideoH265PictureParameterSet     stdPps;
    StdVideoH265ScalingLists            ppsStdScalingLists;
};

class StdVideoPictureParametersSet : public VkParserVideoRefCountBase
{
public:

    enum ItemType {
        PPS_TYPE = 0,
        SPS_TYPE,
        VPS_TYPE,
        NUM_OF_TYPES,
        INVALID_TYPE,
    };

    void Update(VkPictureParameters* pPictureParameters, uint32_t updateSequenceCount)
    {
        switch (pPictureParameters->updateType)
        {
            case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
            case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
            {

                if (pPictureParameters->updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_SPS) {
                    m_data.h264Sps.stdSps = *pPictureParameters->pH264Sps;
                    if (pPictureParameters->pH264Sps->pOffsetForRefFrame &&
                               pPictureParameters->pH264Sps->num_ref_frames_in_pic_order_cnt_cycle) {
                        memcpy(m_data.h264Sps.offset_for_ref_frame,
                                pPictureParameters->pH264Sps->pOffsetForRefFrame,
                                     sizeof(m_data.h264Sps.offset_for_ref_frame) *
                                            pPictureParameters->pH264Sps->num_ref_frames_in_pic_order_cnt_cycle);
                        m_data.h264Sps.stdSps.pOffsetForRefFrame = m_data.h264Sps.offset_for_ref_frame;
                    } else {
                        m_data.h264Sps.stdSps.pOffsetForRefFrame = nullptr;
                    }
                    if (pPictureParameters->pH264Sps->pScalingLists) {
                        m_data.h264Sps.spsStdScalingLists = *pPictureParameters->pH264Sps->pScalingLists;
                        m_data.h264Sps.stdSps.pScalingLists = &m_data.h264Sps.spsStdScalingLists;
                    }
                    if (pPictureParameters->pH264Sps->pSequenceParameterSetVui) {
                        m_data.h264Sps.stdVui = *pPictureParameters->pH264Sps->pSequenceParameterSetVui;
                        m_data.h264Sps.stdSps.pSequenceParameterSetVui = &m_data.h264Sps.stdVui;
                        if (pPictureParameters->pH264Sps->pSequenceParameterSetVui->pHrdParameters) {
                            m_data.h264Sps.stdHrdParameters = *pPictureParameters->pH264Sps->pSequenceParameterSetVui->pHrdParameters;
                            m_data.h264Sps.stdVui.pHrdParameters = &m_data.h264Sps.stdHrdParameters;
                        } else {
                            m_data.h264Sps.stdVui.pHrdParameters = nullptr;
                        }
                    }
                } else if (pPictureParameters->updateType ==  VK_PICTURE_PARAMETERS_UPDATE_H264_PPS ) {
                    m_data.h264Pps.stdPps = *pPictureParameters->pH264Pps;
                    if (pPictureParameters->pH264Pps->pScalingLists) {
                        m_data.h264Pps.ppsStdScalingLists = *pPictureParameters->pH264Pps->pScalingLists;
                        m_data.h264Pps.stdPps.pScalingLists = &m_data.h264Pps.ppsStdScalingLists;
                    }
                }
            }
            break;
            case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
            case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
            case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
            {
                if (pPictureParameters->updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_VPS) {
                    m_data.h265Vps.stdVps = *pPictureParameters->pH265Vps;

                    if (pPictureParameters->pH265Vps->pDecPicBufMgr != 0) {
                        m_data.h265Vps.stdDecPicBufMgr = *pPictureParameters->pH265Vps->pDecPicBufMgr;
                        m_data.h265Vps.stdVps.pDecPicBufMgr = &m_data.h265Vps.stdDecPicBufMgr;
                    }

                    if (pPictureParameters->pH265Vps->pProfileTierLevel != 0) {
                        m_data.h265Vps.stdProfileTierLevel = *pPictureParameters->pH265Vps->pProfileTierLevel;
                        m_data.h265Vps.stdVps.pProfileTierLevel = &m_data.h265Vps.stdProfileTierLevel;
                    }

                    // FIXME: StdVideoH265HrdParameters is currently unsupported
                    m_data.h265Vps.stdVps.pHrdParameters = nullptr;

                } else if (pPictureParameters->updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_SPS) {
                    m_data.h265Sps.stdSps = *pPictureParameters->pH265Sps;
                    if (pPictureParameters->pH265Sps->pScalingLists) {
                        m_data.h265Sps.spsStdScalingLists = *pPictureParameters->pH265Sps->pScalingLists;
                        m_data.h265Sps.stdSps.pScalingLists = &m_data.h265Sps.spsStdScalingLists;
                    }
                    if (pPictureParameters->pH265Sps->pSequenceParameterSetVui) {
                        m_data.h265Sps.stdVui = *pPictureParameters->pH265Sps->pSequenceParameterSetVui;
                        m_data.h265Sps.stdSps.pSequenceParameterSetVui = &m_data.h265Sps.stdVui;
                    }

                } else if (pPictureParameters->updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_PPS) {
                    m_data.h265Pps.stdPps = *pPictureParameters->pH265Pps;
                    if (pPictureParameters->pH265Pps->pScalingLists) {
                        m_data.h265Pps.ppsStdScalingLists = *pPictureParameters->pH265Pps->pScalingLists;
                        m_data.h265Pps.stdPps.pScalingLists = &m_data.h265Pps.ppsStdScalingLists;
                    }
                }
            }
            break;
            default:
                assert(!"Invalid Parser format");
        }

        m_updateSequenceCount = updateSequenceCount;
    }

    int32_t GetVpsId(bool& isVps) const {
        isVps = false;
        switch (m_updateType) {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
            break; // h.264 does not support VPS
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
            isVps = true;
            return m_data.h265Vps.stdVps.vps_video_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
            return m_data.h265Sps.stdSps.sps_video_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
            return m_data.h265Pps.stdPps.sps_video_parameter_set_id;
        default:
            assert("!Invalid STD type");
        }
        return -1;
    }

    int32_t GetSpsId(bool& isSps) const {
        isSps = false;
        switch (m_updateType) {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
            isSps = true;
            return m_data.h264Sps.stdSps.seq_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
            return m_data.h264Pps.stdPps.seq_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
            break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
            isSps = true;
            return m_data.h265Sps.stdSps.sps_seq_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
            return m_data.h265Pps.stdPps.pps_seq_parameter_set_id;
        default:
            assert("!Invalid STD type");
        }
        return -1;
    }

    int32_t GetPpsId(bool& isPps) const {
        isPps = false;
        switch (m_updateType) {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
            break;
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
            isPps = true;
            return m_data.h264Pps.stdPps.pic_parameter_set_id;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
            break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
            isPps = true;
            return m_data.h265Pps.stdPps.pps_pic_parameter_set_id;
        default:
            assert("!Invalid STD type");
        }
        return -1;
    }

    static StdVideoPictureParametersSet* Create(VkPictureParameters* pPictureParameters, uint64_t updateSequenceCount)
    {
        StdVideoPictureParametersSet* pNewSet = new StdVideoPictureParametersSet(pPictureParameters->updateType);

        pNewSet->Update(pPictureParameters, (uint32_t)updateSequenceCount);

        return pNewSet;
    }

    static StdVideoPictureParametersSet* StdVideoPictureParametersSetFromBase(VkParserVideoRefCountBase* pBase ) {
        if (!pBase) {
            return NULL;
        }
        StdVideoPictureParametersSet* pPictureParameters = static_cast<StdVideoPictureParametersSet*>(pBase);
        if (m_refClassId == pPictureParameters->m_classId) {
            return pPictureParameters;
        }
        assert(!"Invalid StdVideoPictureParametersSet from base");
        return nullptr;
    }

    virtual int32_t AddRef()
    {
        return ++m_refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --m_refCount;
        // Destroy the device if refcount reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

private:
    static const char*                   m_refClassId;
    const char*                          m_classId;
    std::atomic<int32_t>                 m_refCount;
public:
    VkParserPictureParametersUpdateType  m_updateType;
    ItemType                             m_itemType;
    union {
        SpsVideoH264PictureParametersSet h264Sps;
        PpsVideoH264PictureParametersSet h264Pps;
        SpsVideoH265VideoParametersSet   h265Vps;
        SpsVideoH265PictureParametersSet h265Sps;
        PpsVideoH265PictureParametersSet h265Pps;
    } m_data;
    uint32_t                                         m_updateSequenceCount;
    VkSharedBaseObj<StdVideoPictureParametersSet>    m_parent;        // SPS or PPS parent
    VkSharedBaseObj<VkParserVideoRefCountBase>       m_vkObjectOwner; // VkParserVideoPictureParameters
    VkSharedBaseObj<VkParserVideoRefCountBase>       m_videoSession;  // NvVideoSession
private:

    StdVideoPictureParametersSet(VkParserPictureParametersUpdateType updateType)
    : m_classId(m_refClassId),
      m_refCount(0),
      m_updateType(updateType),
      m_data(),
      m_updateSequenceCount(0),
      m_parent(),
      m_vkObjectOwner(),
      m_videoSession(nullptr)
    {
        switch (m_updateType) {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
            m_itemType = PPS_TYPE;
            break;
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
            m_itemType = SPS_TYPE;
            break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
            m_itemType = VPS_TYPE;
            break;
        default:
            m_itemType = INVALID_TYPE;
            assert("!Invalid STD type");
        }
    }

    ~StdVideoPictureParametersSet()
    {
        m_vkObjectOwner = nullptr;
        m_videoSession = nullptr;
    }

};

#endif /* _NVVKDECODER_STDVIDEOPICTUREPARAMETERSSET_H_ */
