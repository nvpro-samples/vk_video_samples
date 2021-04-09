/***************************************************************************\
|*                                                                           *|
|*      Copyright 2018-2020 NVIDIA Corporation.  All rights reserved.        *|
|*                                                                           *|
|*   THE SOFTWARE AND INFORMATION CONTAINED HEREIN IS PROPRIETARY AND        *|
|*   CONFIDENTIAL TO NVIDIA CORPORATION. THIS SOFTWARE IS FOR INTERNAL USE   *|
|*   ONLY AND ANY REPRODUCTION OR DISCLOSURE TO ANY PARTY OUTSIDE OF NVIDIA  *|
|*   IS STRICTLY PROHIBITED.                                                 *|
|*                                                                           *|
\***************************************************************************/

#ifndef _NVVIDEOPROFILE_H_
#define _NVVIDEOPROFILE_H_

#include <assert.h>
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"

typedef enum StdChromaFormatIdc {
    chroma_format_idc_monochrome  = std_video_h264_chroma_format_idc_monochrome,
    chroma_format_idc_420  = std_video_h264_chroma_format_idc_420,
    chroma_format_idc_422  = std_video_h264_chroma_format_idc_422,
    chroma_format_idc_444  = std_video_h264_chroma_format_idc_444,
} StdChromaFormatIdc;

#if defined(__linux) || defined(__linux__) || defined(linux)
static_assert((uint32_t)chroma_format_idc_monochrome == (uint32_t)std_video_h265_chroma_format_idc_monochrome);
static_assert((uint32_t)chroma_format_idc_420        == (uint32_t)std_video_h265_chroma_format_idc_420);
static_assert((uint32_t)chroma_format_idc_422        == (uint32_t)std_video_h265_chroma_format_idc_422);
static_assert((uint32_t)chroma_format_idc_444        == (uint32_t)std_video_h265_chroma_format_idc_444);
#endif

class nvVideoProfile
{

public:

    void PopulateProfileExt(VkBaseInStructure* pVideoProfileExt)
    {

        if (m_profile.videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
            VkVideoDecodeH264ProfileEXT* pProfileExt = (VkVideoDecodeH264ProfileEXT*)pVideoProfileExt;
            if (pProfileExt && (pProfileExt->sType != VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_EXT)) {
                m_profile.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                return;
            }
            if (pProfileExt) {
                m_h264DecodeProfile = *pProfileExt;
            } else {
                //  Use default ext profile parameters
                m_h264DecodeProfile.sType         = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_EXT;
                m_h264DecodeProfile.stdProfileIdc = std_video_h264_profile_idc_main;
                m_h264DecodeProfile.fieldLayout   = VK_VIDEO_DECODE_H264_FIELD_LAYOUT_LINE_INTERLACED_PLANE_BIT_EXT;
            }
            m_profile.pNext = &m_h264DecodeProfile;
            m_h264DecodeProfile.pNext = NULL;
        } else if (m_profile.videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
            VkVideoDecodeH265ProfileEXT* pProfileExt = (VkVideoDecodeH265ProfileEXT*)pVideoProfileExt;
            if (pProfileExt && (pProfileExt->sType != VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_EXT)) {
                m_profile.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                return;
            }
            if (pProfileExt) {
                m_h265DecodeProfile = *pProfileExt;
            } else {
                //  Use default ext profile parameters
                m_h265DecodeProfile.sType         = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_EXT;
                m_h265DecodeProfile.stdProfileIdc = std_video_h265_profile_idc_main;
            }
            m_profile.pNext = &m_h265DecodeProfile;
            m_h265DecodeProfile.pNext = NULL;
        } else if (m_profile.videoCodecOperation == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT) {
            VkVideoEncodeH264ProfileEXT* pProfileExt = (VkVideoEncodeH264ProfileEXT*)pVideoProfileExt;
            if (pProfileExt && (pProfileExt->sType != VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_EXT)) {
                m_profile.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                return;
            }
            if (pProfileExt) {
                m_h264EncodeProfile = *pProfileExt;
            } else {
                //  Use default ext profile parameters
                m_h264DecodeProfile.sType         = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_EXT;
                m_h264DecodeProfile.stdProfileIdc = std_video_h264_profile_idc_main;
            }
            m_profile.pNext = &m_h264EncodeProfile;
            m_h264EncodeProfile.pNext = NULL;
        }
    }

    nvVideoProfile(const VkVideoProfileKHR* pVideoProfile)
        : m_profile(*pVideoProfile)
    {

        PopulateProfileExt((VkBaseInStructure*)pVideoProfile->pNext);
    }

    nvVideoProfile( VkVideoCodecOperationFlagBitsKHR videoCodecOperation, VkBaseInStructure* pVideoProfileExt = NULL,
                          VkVideoChromaSubsamplingFlagsKHR chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_INVALID_BIT_KHR,
                          VkVideoComponentBitDepthFlagsKHR lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR,
                          VkVideoComponentBitDepthFlagsKHR chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR)
        : m_profile({VK_STRUCTURE_TYPE_VIDEO_PROFILE_KHR, NULL,
                     videoCodecOperation, chromaSubsampling, lumaBitDepth, chromaBitDepth})
    {
        PopulateProfileExt(pVideoProfileExt);
    }

    VkVideoCodecOperationFlagBitsKHR GetCodecType() const
    {
        return m_profile.videoCodecOperation;
    }

    operator bool() const
    {
        return (m_profile.sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_KHR);
    }

    const VkVideoProfileKHR* GetProfile() const
    {
        if (m_profile.sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_KHR) {
            return &m_profile;
        } else {
            return NULL;
        }
    }


    const VkVideoDecodeH264ProfileEXT* GetDecodeH264Profile() const
    {
        if (m_h264DecodeProfile.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_EXT) {
            return &m_h264DecodeProfile;
        } else {
            return NULL;
        }
    }

    const VkVideoDecodeH265ProfileEXT* GetDecodeH265Profile() const
    {
        if (m_h265DecodeProfile.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_EXT) {
            return &m_h265DecodeProfile;
        } else {
            return NULL;
        }
    }

    const VkVideoEncodeH264ProfileEXT* GetEncodeH264Profile() const
    {
        if (m_h264DecodeProfile.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_EXT) {
            return &m_h264EncodeProfile;
        } else {
            return NULL;
        }
    }

    bool copyProfile(const nvVideoProfile& src)
    {
        if (!src) {
            return false;
        }

        m_profile = src.m_profile;
        m_profile.pNext = NULL;

        PopulateProfileExt((VkBaseInStructure*)src.m_profile.pNext);

        return true;
    }

    nvVideoProfile(const nvVideoProfile& other)
    {
        copyProfile(other);
    }

    nvVideoProfile& operator = (const nvVideoProfile& other)
    {
        copyProfile(other);
        return *this;
    }

    VkVideoChromaSubsamplingFlagsKHR GetColorSubsampling() const
    {
        return m_profile.chromaSubsampling;
    }

    StdChromaFormatIdc GetNvColorSubsampling() const
    {
        if (m_profile.chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR ) {
            return chroma_format_idc_monochrome;
        } else if (m_profile.chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR ) {
            return chroma_format_idc_420;
        } else if (m_profile.chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR ) {
            return chroma_format_idc_422;
        } else if (m_profile.chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR ) {
            return chroma_format_idc_444;
        }

        return chroma_format_idc_monochrome;
    }

    uint32_t GetLumaBitDepthMinus8() const
    {
        if (m_profile.lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ) {
            return 8 - 8;
        } else if (m_profile.lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ) {
            return 10 - 8;
        } else if (m_profile.lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR ) {
            return 12 - 8;
        }
        return 0;
    }

    uint32_t GetChromaBitDepthMinus8() const
    {
        if (m_profile.chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ) {
            return 8 - 8;
        } else if (m_profile.chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ) {
            return 10 - 8;
        } else if (m_profile.chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR ) {
            return 12 - 8;
        }
        return 0;
    }

    bool is16BitFormat() const
    {
        return !!GetLumaBitDepthMinus8() || !!GetChromaBitDepthMinus8();
    }

    static StdChromaFormatIdc GetVideoChromaFormatFromVkFormat(VkFormat format)
    {
        StdChromaFormatIdc videoChromaFormat = chroma_format_idc_420;
        switch ((uint32_t)format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R10X6_UNORM_PACK16:
        case VK_FORMAT_R12X4_UNORM_PACK16:
            videoChromaFormat = chroma_format_idc_monochrome;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
            videoChromaFormat = chroma_format_idc_420;
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
            videoChromaFormat = chroma_format_idc_422;
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT:
        case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM_EXT:
            videoChromaFormat = chroma_format_idc_444;
            break;
        default:
            assert(0);
        }

        return videoChromaFormat;
    }

private:
    VkVideoProfileKHR m_profile;
    union
    {
        VkVideoDecodeH264ProfileEXT m_h264DecodeProfile;
        VkVideoDecodeH265ProfileEXT m_h265DecodeProfile;
        VkVideoEncodeH264ProfileEXT m_h264EncodeProfile;
    };
};



#endif /* _NVVIDEOPROFILE_H_ */
