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

#ifndef _VKVIDEODECODER_VKPARSERVIDEOPICTUREPARAMETERS_H_
#define _VKVIDEODECODER_VKPARSERVIDEOPICTUREPARAMETERS_H_

#include <bitset>

class VkParserVideoPictureParameters : public VkVideoRefCountBase {
public:
    static const uint32_t MAX_VPS_IDS =  16;
    static const uint32_t MAX_SPS_IDS =  32;
    static const uint32_t MAX_PPS_IDS = 256;

    //! Increment the reference count by 1.
    virtual int32_t AddRef();

    //! Decrement the reference count by 1. When the reference count
    //! goes to 0 the object is automatically destroyed.
    virtual int32_t Release();

    static VkParserVideoPictureParameters* VideoPictureParametersFromBase(VkVideoRefCountBase* pBase ) {
        if (!pBase) {
            return NULL;
        }
        VkParserVideoPictureParameters* pPictureParameters = static_cast<VkParserVideoPictureParameters*>(pBase);
        if (m_refClassId == pPictureParameters->m_classId) {
            return pPictureParameters;
        }
        assert(!"Invalid VkParserVideoPictureParameters from base");
        return nullptr;
    }

    static VkParserVideoPictureParameters* Create(const VulkanDeviceContext* vkDevCtx,
                                                  VkSharedBaseObj<NvVideoSession>& videoSession,
                                                  const StdVideoPictureParametersSet* pVpsStdPictureParametersSet,
                                                  const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                                                  const StdVideoPictureParametersSet* pPpsStdPictureParametersSet,
                                                  VkParserVideoPictureParameters* pTemplate);

    static int32_t PopulateH264UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                     VkVideoDecodeH264SessionParametersAddInfoKHR& h264SessionParametersAddInfo);

    static int32_t PopulateH265UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                     VkVideoDecodeH265SessionParametersAddInfoKHR& h265SessionParametersAddInfo);

    VkResult Update(const StdVideoPictureParametersSet* pVpsStdPictureParametersSet,
                    const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                    const StdVideoPictureParametersSet* pPpsStdPictureParametersSet);

    operator VkVideoSessionParametersKHR() const {
        return m_sessionParameters;
    }

    VkVideoSessionParametersKHR GetVideoSessionParametersKHR() const {
        return m_sessionParameters;
    }

    int32_t GetId() const { return m_Id; }

    bool HasVpsId(uint32_t vpsId) const {
        return m_vpsIdsUsed[vpsId];
    }

    bool HasSpsId(uint32_t spsId) const {
        return m_spsIdsUsed[spsId];
    }

    bool HasPpsId(uint32_t ppsId) const {
        return m_ppsIdsUsed[ppsId];
    }


protected:
    VkParserVideoPictureParameters(const VulkanDeviceContext* vkDevCtx)
        : m_classId(m_refClassId),
          m_Id(-1),
          m_refCount(0),
          m_vkDevCtx(vkDevCtx),
          m_videoSession(),
          m_sessionParameters() { }

    virtual ~VkParserVideoPictureParameters();

private:
    static const char*              m_refClassId;
    static int32_t                  m_currentId;
    const char*                     m_classId;
    int32_t                         m_Id;
    std::atomic<int32_t>            m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<NvVideoSession> m_videoSession;
    VkVideoSessionParametersKHR     m_sessionParameters;
    std::bitset<MAX_VPS_IDS>        m_vpsIdsUsed;
    std::bitset<MAX_SPS_IDS>        m_spsIdsUsed;
    std::bitset<MAX_PPS_IDS>        m_ppsIdsUsed;
};

#endif /* _VKVIDEODECODER_VKPARSERVIDEOPICTUREPARAMETERS_H_ */
