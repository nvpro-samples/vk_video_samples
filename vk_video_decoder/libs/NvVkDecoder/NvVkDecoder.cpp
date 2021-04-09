/*
* Copyright 2020 NVIDIA Corporation.
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

#include <algorithm>
#include <chrono>
#include <iostream>


#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "NvVkDecoder/NvVkDecoder.h"
#include "vulkan_interfaces.h"

#define GPU_ALIGN(x) (((x) + 0xff) & ~0xff)

const char* NvVkDecoder::GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec)
{
    static struct {
        VkVideoCodecOperationFlagBitsKHR eCodec;
        const char* name;
    } aCodecName[] = {
        { VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR, "Invalid" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT, "AVC/H.264" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT, "H.265/HEVC" },
#ifdef VK_EXT_video_decode_vp9
        { VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9" },
#endif // VK_EXT_video_decode_vp9
#ifdef VK_EXT_video_decode_av1
        { VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1" },
#endif // VK_EXT_video_decode_av1
    };

    for (unsigned i = 0; i < sizeof(aCodecName) / sizeof(aCodecName[0]); i++) {
        if (codec == aCodecName[i].eCodec) {
            return aCodecName[codec].name;
        }
    }

    return "Unknown";
}

const char* NvVkDecoder::GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat)
{

    switch (chromaFormat) {
    case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
        return "YCbCr 400 (Monochrome)";
    case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
        return "YCbCr 420";
    case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
        return "YCbCr 422";
    case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
        return "YCbCr 444";
    default:
        assert(!"Unknown Chroma sub-sampled format");
    };

    return "Unknown";
}

uint32_t NvVkDecoder::GetNumDecodeSurfaces(VkVideoCodecOperationFlagBitsKHR codec, uint32_t minNumDecodeSurfaces, uint32_t width,
    uint32_t height)
{

#ifdef VK_EXT_video_decode_vp9
    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
        return 12;
    }
#endif // VK_EXT_video_decode_vp9

    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        // H264: minNumDecodeSurfaces plus 4 for non-reference render target plus 4 for display
        return minNumDecodeSurfaces + 4 + 4;
    }

    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
        // ref HEVC spec: A.4.1 General tier and level limits
        // currently assuming level 6.2, 8Kx4K
        int MaxLumaPS = 35651584;
        int MaxDpbPicBuf = 6;
        int PicSizeInSamplesY = (int)(width * height);
        int MaxDpbSize;
        if (PicSizeInSamplesY <= (MaxLumaPS >> 2))
            MaxDpbSize = MaxDpbPicBuf * 4;
        else if (PicSizeInSamplesY <= (MaxLumaPS >> 1))
            MaxDpbSize = MaxDpbPicBuf * 2;
        else if (PicSizeInSamplesY <= ((3 * MaxLumaPS) >> 2))
            MaxDpbSize = (MaxDpbPicBuf * 4) / 3;
        else
            MaxDpbSize = MaxDpbPicBuf;
        return (std::min)(MaxDpbSize, 16) + 4;
    }

    return 8;
}

VkFormat NvVkDecoder::CodecGetVkFormat(VkVideoChromaSubsamplingFlagBitsKHR chromaFormatIdc, int bitDepthLumaMinus8, bool isSemiPlanar)
{
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    switch (chromaFormatIdc) {
    case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
        switch (bitDepthLumaMinus8) {
        case 0:
            vkFormat = VK_FORMAT_R8_UNORM;
            break;
        case 2:
            vkFormat = VK_FORMAT_R10X6_UNORM_PACK16;
            break;
        case 4:
            vkFormat = VK_FORMAT_R12X4_UNORM_PACK16;
            break;
        default:
            assert(0);
        }
        break;
    case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
        switch (bitDepthLumaMinus8) {
        case 0:
            vkFormat = isSemiPlanar ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
            break;
        case 2:
            vkFormat = isSemiPlanar ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                                    : VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
            break;
        case 4:
            vkFormat = isSemiPlanar ? VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16
                                    : VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16;
            break;
        default:
            assert(0);
        }
        break;
    case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
        switch (bitDepthLumaMinus8) {
        case 0:
            vkFormat = isSemiPlanar ? VK_FORMAT_G8_B8R8_2PLANE_422_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM;
            break;
        case 2:
            vkFormat = isSemiPlanar ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16
                                    : VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16;
            break;
        case 4:
            vkFormat = isSemiPlanar ? VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16
                                    : VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16;
            break;
        default:
            assert(0);
        }
        break;
    case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
        switch (bitDepthLumaMinus8) {
        case 0:
            vkFormat = isSemiPlanar ? VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT : VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
            break;
        case 2:
            vkFormat = isSemiPlanar ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT
                                    : VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16;
            break;
        case 4:
            vkFormat = isSemiPlanar ? VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT
                                    : VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16;
            break;
        default:
            assert(0);
        }
        break;
    default:
        assert(0);
    }

    return vkFormat;
}

const char* NvVkDecoder::CodecToName(VkVideoCodecOperationFlagBitsKHR codec)
{
    switch ((int32_t)codec) {
    case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
        return "H264";
    case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
        return "H265";
#ifdef VK_EXT_video_decode_vp9
    case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
        return "VP9";
#endif // VK_EXT_video_decode_vp9
#ifdef VK_EXT_video_decode_av1
    case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
        return "AV1";
#endif // VK_EXT_video_decode_av1
    default:;
    }
    return "UNKNON";
}

class VideoDecodeProfiles {
public:
    VideoDecodeProfiles()
        : m_maxNumProfiles(0)
        , m_pDecodeProfiles()
        , m_pH264Profiles()
        , m_pH265Profiles()
    {
    }

    ~VideoDecodeProfiles() { DestroyProfiles(); }

    void DestroyProfiles()
    {
        if (m_pDecodeProfiles) {
            delete[] m_pDecodeProfiles;
        }

        m_maxNumProfiles = 0;
        m_pDecodeProfiles = NULL;
        m_pH264Profiles = NULL;
        m_pH265Profiles = NULL;
    }

    static void DumpFormatProfiles(VkVideoProfileKHR* pVideoProfile);
    static void DumpH264Profiles(VkVideoDecodeH264ProfileEXT* pH264Profiles);
    static void DumpH265Profiles(VkVideoDecodeH265ProfileEXT* m_pH265Profiles);

    VkResult InitProfiles(VkPhysicalDevice vkPhysicalDev, uint32_t vkVideoDecodeQueueFamily, const nvVideoProfile* pProfile)
    {
        VkVideoCodecOperationFlagsKHR videoCodecs = vk::GetSupportedCodecs(vkPhysicalDev,
            (int32_t*)&vkVideoDecodeQueueFamily,
            VK_QUEUE_VIDEO_DECODE_BIT_KHR,
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT);

        if (!(videoCodecs & pProfile->GetCodecType())) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }

        const VkVideoProfileKHR* pInVideoProfile = pProfile->GetProfile();

        // Get the actual count
        uint32_t decodeProfileCount = 0;
        VkResult result = vk::GetPhysicalDeviceVideoCodecProfilesNV(vkPhysicalDev, pInVideoProfile,
            &decodeProfileCount, NULL);
        if ((result != VK_SUCCESS) && (result != VK_INCOMPLETE)) {
            return result;
        }

        VkVideoProfileKHR* pDecodeProfiles = InitProfiles(pProfile, decodeProfileCount);
        if (!pDecodeProfiles) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        result = vk::GetPhysicalDeviceVideoCodecProfilesNV(vkPhysicalDev, pInVideoProfile,
            &decodeProfileCount, pDecodeProfiles);
        return result;
    }

    VkVideoProfileKHR* getProfiles() { return m_pDecodeProfiles; }

    VkVideoProfileKHR* getProfile(uint32_t profileNum)
    {
        if (profileNum < m_maxNumProfiles) {
            return &m_pDecodeProfiles[profileNum];
        }
        return NULL;
    }

    VkVideoDecodeH264ProfileEXT* getH264Profiles()
    {
        if (m_pDecodeProfiles && m_maxNumProfiles) {
            return m_pH264Profiles;
        }
        return NULL;
    }

    VkVideoDecodeH264ProfileEXT* getH264Profile(uint32_t profileNum)
    {
        VkVideoDecodeH264ProfileEXT* pH264Profiles = getH264Profiles();
        if (pH264Profiles && (profileNum < m_maxNumProfiles)) {
            return &pH264Profiles[profileNum];
        }
        return NULL;
    }

    VkVideoDecodeH265ProfileEXT* getH265Profiles()
    {
        if (m_pDecodeProfiles && m_maxNumProfiles) {
            return m_pH265Profiles;
        }
        return NULL;
    }

    VkVideoDecodeH265ProfileEXT* getH265Profile(uint32_t profileNum)
    {
        VkVideoDecodeH265ProfileEXT* pH265Profiles = getH265Profiles();
        if (pH265Profiles && (profileNum < m_maxNumProfiles)) {
            return &pH265Profiles[profileNum];
        }
        return NULL;
    }

    void DumpProfiles(uint32_t firstProfileId = 0, uint32_t lastProfileId = (uint32_t)-1)
    {
        if (!m_pDecodeProfiles || !(firstProfileId < m_maxNumProfiles)) {
            return;
        }

        if ((lastProfileId == (uint32_t)-1) || !(lastProfileId < m_maxNumProfiles)) {
            lastProfileId = m_maxNumProfiles;
        } else {
            lastProfileId++;
        }

        for (uint32_t p = firstProfileId; p < lastProfileId; p++) {
            switch ((int32_t)m_pDecodeProfiles[p].videoCodecOperation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
                std::cout << "\t\tH264 profile: ";
                DumpFormatProfiles(&m_pDecodeProfiles[p]);
                if (m_pH264Profiles)
                    DumpH264Profiles(&m_pH264Profiles[p]);
                std::cout << std::endl;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
                std::cout << "\t\tH265 profile: ";
                DumpFormatProfiles(&m_pDecodeProfiles[p]);
                if (m_pH265Profiles)
                    DumpH265Profiles(&m_pH265Profiles[p]);
                std::cout << std::endl;
                break;
#ifdef VK_EXT_video_decode_vp9
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
#endif // VK_EXT_video_decode_vp9
            default:;
            }
        }
    }

    uint32_t GetNumProfiles() { return m_maxNumProfiles; }

private:
    VkVideoProfileKHR* InitProfiles(const nvVideoProfile* pProfile, uint32_t maxNumProfiles)
    {
        DestroyProfiles();

        size_t profilesSize = sizeof(VkVideoProfileKHR);
        if (pProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
            profilesSize += sizeof(VkVideoDecodeH264ProfileEXT);
        } else if (pProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
            profilesSize += sizeof(VkVideoDecodeH265ProfileEXT);
        }

        unsigned char* pProfileMemory = new unsigned char[maxNumProfiles * profilesSize];

        if (pProfileMemory) {
            m_maxNumProfiles = maxNumProfiles;

            m_pDecodeProfiles = (VkVideoProfileKHR*)pProfileMemory;
            pProfileMemory += (sizeof(VkVideoProfileKHR) * maxNumProfiles);
            for (unsigned p = 0; p < maxNumProfiles; p++) {
                m_pDecodeProfiles[p] = *pProfile->GetProfile();
                m_pDecodeProfiles[p].pNext = NULL;
            }
            if (pProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
                m_pH264Profiles = (VkVideoDecodeH264ProfileEXT*)pProfileMemory;
                pProfileMemory += (sizeof(VkVideoDecodeH264ProfileEXT) * maxNumProfiles);
                for (unsigned p = 0; p < maxNumProfiles; p++) {
                    m_pH264Profiles[p] = *pProfile->GetDecodeH264Profile();
                    m_pH264Profiles[p].pNext = NULL;
                    m_pDecodeProfiles[p].pNext = &m_pH264Profiles[p];
                }
            } else if (pProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
                m_pH265Profiles = (VkVideoDecodeH265ProfileEXT*)pProfileMemory;
                pProfileMemory += (sizeof(VkVideoDecodeH265ProfileEXT) * maxNumProfiles);
                for (unsigned p = 0; p < maxNumProfiles; p++) {
                    m_pH265Profiles[p] = *pProfile->GetDecodeH265Profile();
                    m_pH265Profiles[p].pNext = NULL;
                    m_pDecodeProfiles[p].pNext = &m_pH265Profiles[p];
                }
            }
        }
        return m_pDecodeProfiles;
    }

private:
    uint32_t m_maxNumProfiles;
    VkVideoProfileKHR* m_pDecodeProfiles;
    VkVideoDecodeH264ProfileEXT* m_pH264Profiles;
    VkVideoDecodeH265ProfileEXT* m_pH265Profiles;
};

void VideoDecodeProfiles::DumpFormatProfiles(VkVideoProfileKHR* pVideoProfile)
{
    // formatProfile info based on supported chroma_format_idc
    if (pVideoProfile->chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR) {
        std::cout << "MONO, ";
    }
    if (pVideoProfile->chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR) {
        std::cout << " 420, ";
    }
    if (pVideoProfile->chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR) {
        std::cout << " 422, ";
    }
    if (pVideoProfile->chromaSubsampling & VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR) {
        std::cout << " 444, ";
    }

    // Profile info based on max bit_depth_luma_minus8
    if (pVideoProfile->lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR) {
        std::cout << "LUMA:   8-bit, ";
    }
    if (pVideoProfile->lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR) {
        std::cout << "LUMA:  10-bit, ";
    }
    if (pVideoProfile->lumaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR) {
        std::cout << "LUMA:  12-bit, ";
    }

    // Profile info based on max bit_depth_chroma_minus8
    if (pVideoProfile->chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR) {
        std::cout << "CHROMA: 8-bit, ";
    }
    if (pVideoProfile->chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR) {
        std::cout << "CHROMA:10-bit, ";
    }
    if (pVideoProfile->chromaBitDepth & VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR) {
        std::cout << "CHROMA:12-bit,";
    }
}

void VideoDecodeProfiles::DumpH264Profiles(VkVideoDecodeH264ProfileEXT* pH264Profiles)
{
    switch (pH264Profiles->stdProfileIdc) {
    case std_video_h264_profile_idc_baseline:
        std::cout << "BASELINE, ";
        break;
    case std_video_h264_profile_idc_main:
        std::cout << "MAIN, ";
        break;
    case std_video_h264_profile_idc_high:
        std::cout << "HIGH, ";
        break;
    case std_video_h264_profile_idc_high_444_predictive:
        std::cout << "HIGH_444_PREDICTIVE, ";
        break;
    default:
        std::cout << "UNKNOWN PROFILE, ";
        break;
    }
}

void VideoDecodeProfiles::DumpH265Profiles(VkVideoDecodeH265ProfileEXT* pH265Profiles)
{
    switch (pH265Profiles->stdProfileIdc) {
    case std_video_h265_profile_idc_main:
        std::cout << "MAIN, ";
        break;
    case std_video_h265_profile_idc_main_10:
        std::cout << "MAIN_10, ";
        break;
    case std_video_h265_profile_idc_main_still_picture:
        std::cout << "MAIN_STILL_PICTURE, ";
        break;
    case std_video_h265_profile_idc_format_range_extensions:
        std::cout << "FORMAT_RANGE_EXTENSIONS, ";
        break;
    case std_video_h265_profile_idc_scc_extensions:
        std::cout << "SCC_EXTENSIONS, ";
        break;
    default:
        std::cout << "UNKNOWN PROFILE, ";
        break;
    }
}

/* Callback function to be registered for getting a callback when decoding of
 * sequence starts. Return value from HandleVideoSequence() are interpreted as :
 *  0: fail, 1: suceeded, > 1: override dpb size of parser (set by
 * nvVideoParseParameters::ulMaxNumDecodeSurfaces while creating parser)
 */
int32_t NvVkDecoder::StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat)
{
    std::cout << "Video Input Information" << std::endl
              << "\tCodec        : " << GetVideoCodecString(pVideoFormat->codec) << std::endl
              << "\tFrame rate   : " << pVideoFormat->frame_rate.numerator << "/" << pVideoFormat->frame_rate.denominator << " = "
              << 1.0 * pVideoFormat->frame_rate.numerator / pVideoFormat->frame_rate.denominator << " fps" << std::endl
              << "\tSequence     : " << (pVideoFormat->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
              << "\tCoded size   : [" << pVideoFormat->coded_width << ", " << pVideoFormat->coded_height << "]" << std::endl
              << "\tDisplay area : [" << pVideoFormat->display_area.left << ", " << pVideoFormat->display_area.top << ", "
              << pVideoFormat->display_area.right << ", " << pVideoFormat->display_area.bottom << "]" << std::endl
              << "\tChroma       : " << GetVideoChromaFormatString(pVideoFormat->chromaSubsampling) << std::endl
              << "\tBit depth    : " << pVideoFormat->bit_depth_luma_minus8 + 8 << std::endl;

    m_numDecodeSurfaces = GetNumDecodeSurfaces(pVideoFormat->codec, pVideoFormat->minNumDecodeSurfaces, pVideoFormat->coded_width,
        pVideoFormat->coded_height);
    // assert(m_numDecodeSurfaces <= 17);

    VkResult result;
#ifndef NV_RMAPI_TEGRA // TODO: Fix the Vulkan loader to support Vulkan Video on ARM
    // Use direct video codec API, until the Vulkan loader is fixed.

    VkVideoCodecOperationFlagsKHR videoCodecs = vk::GetSupportedCodecs(m_pVulkanDecodeContext.physicalDev,
        (int32_t*)&m_pVulkanDecodeContext.videoDecodeQueueFamily,
        VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT);
    assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR);

    if (m_dumpDecodeData)
        std::cout << "\t" << std::hex << videoCodecs << " HW codec types are available: " << std::endl;
    for (uint32_t i = 0; videoCodecs; i++) {
        const VkVideoCodecOperationFlagsKHR videoCodecsMask = (1 << i);
        if (!(videoCodecs & videoCodecsMask)) {
            continue;
        }

        VkVideoCodecOperationFlagBitsKHR videoCodec = (VkVideoCodecOperationFlagBitsKHR)videoCodecsMask;
        videoCodecs &= ~videoCodecsMask;

        if (m_dumpDecodeData)
            std::cout << "\tcodec " << i << ": " << CodecToName(videoCodec) << std::endl;

        VkVideoDecodeH264ProfileEXT h264ProfilesRequest;
        VkVideoDecodeH265ProfileEXT h265ProfilesRequest;
        VkBaseInStructure* pVideoProfileExt = NULL;

        if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
            h264ProfilesRequest.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_EXT;
            h264ProfilesRequest.pNext = NULL;
            h264ProfilesRequest.stdProfileIdc = std_video_h264_profile_idc_invalid; // Any
            h264ProfilesRequest.fieldLayout = VK_VIDEO_DECODE_H264_FIELD_LAYOUT_LINE_INTERLACED_PLANE_BIT_EXT;
            pVideoProfileExt = (VkBaseInStructure*)&h264ProfilesRequest;
        } else if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
            h265ProfilesRequest.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_EXT;
            h265ProfilesRequest.pNext = NULL;
            h265ProfilesRequest.stdProfileIdc = std_video_h265_profile_idc_invalid; // Any
            pVideoProfileExt = (VkBaseInStructure*)&h265ProfilesRequest;
        }

        nvVideoProfile inProfile(videoCodec, pVideoProfileExt);

        VideoDecodeProfiles decodeProfiles;
        result = decodeProfiles.InitProfiles(m_pVulkanDecodeContext.physicalDev, m_pVulkanDecodeContext.videoDecodeQueueFamily, &inProfile);
        assert(result == VK_SUCCESS);
        decodeProfiles.DumpProfiles();

        if (decodeProfiles.GetNumProfiles() == 0) {
            continue;
        }
    }
#endif // TODO: Fix the Vulkan loader to support Vulkan Video on ARM

    if (m_width && m_height) {
        // CreateDecoder() has been called before, and now there's possible config change
        // Not supported yet.
        assert(false);
    }

    // eCodec has been set in the constructor (for parser). Here it's set again for potential correction
    m_codecType = pVideoFormat->codec;
    m_chromaFormat = pVideoFormat->chromaSubsampling;
    m_bitLumaDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;
    m_bitChromaDepthMinus8 = pVideoFormat->bit_depth_chroma_minus8;
    m_videoFormat = *pVideoFormat;

    // m_outputFormat = pVideoFormat->bit_depth_luma_minus8 ? nvVideoSurfaceFormat_P016 : nvVideoSurfaceFormat_NV12;
    // m_deinterlaceMode = nvVideoDeinterlaceMode_Weave;
    // With PreferNVVID, JPEG is still decoded by CUDA while video is decoded by NVDEC hardware
    m_codedWidth = pVideoFormat->coded_width;
    m_codedHeight = pVideoFormat->coded_height;

    if (!(m_cropRect.r && m_cropRect.b)) {
        m_width = pVideoFormat->display_area.right - pVideoFormat->display_area.left;
        m_height = pVideoFormat->display_area.bottom - pVideoFormat->display_area.top;
        // videoDecodeCreateInfo.ulTargetWidth = pVideoFormat->coded_width;
        // videoDecodeCreateInfo.ulTargetHeight = pVideoFormat->coded_height;
    } else {
        if (m_cropRect.r && m_cropRect.b) {
            // videoDecodeCreateInfo.display_area.left = m_cropRect.l;
            // videoDecodeCreateInfo.display_area.top = m_cropRect.t;
            // videoDecodeCreateInfo.display_area.right = m_cropRect.r;
            // videoDecodeCreateInfo.display_area.bottom = m_cropRect.b;
            m_width = m_cropRect.r - m_cropRect.l;
            m_height = m_cropRect.b - m_cropRect.t;
        }
        // videoDecodeCreateInfo.ulTargetWidth = m_width;
        // videoDecodeCreateInfo.ulTargetHeight = m_height;
    }
    m_surfaceHeight = pVideoFormat->coded_height;
    m_surfaceWidth = pVideoFormat->coded_width;

    std::cout << "Video Decoding Params:" << std::endl
              << "\tNum Surfaces : " << m_numDecodeSurfaces << std::endl
              << "\tCrop         : [" << 0 << ", " << 0 << ", " << 0 << ", " << 0 << "]" << std::endl
              << "\tResize       : " << pVideoFormat->coded_width << "x" << pVideoFormat->coded_height << std::endl;

    uint32_t maxDpbSlotCount = pVideoFormat->maxNumDpbSlots; // This is currently configured by the parser to maxNumDpbSlots from the stream plus 1 for the current slot on the fly

    VkVideoComponentBitDepthFlagsKHR lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
    switch (pVideoFormat->bit_depth_luma_minus8) {
    case 0:
        lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        break;
    case 2:
        lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        break;
    case 4:
        lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
        break;
    default:
        assert(false);
    }

    VkVideoComponentBitDepthFlagsKHR chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
    switch (pVideoFormat->bit_depth_chroma_minus8) {
    case 0:
        chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        break;
    case 2:
        chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        break;
    case 4:
        chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
        break;
    default:
        assert(false);
    }

    assert(VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR == pVideoFormat->chromaSubsampling || VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR == pVideoFormat->chromaSubsampling || VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR == pVideoFormat->chromaSubsampling || VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR == pVideoFormat->chromaSubsampling);

    nvVideoProfile videoProfile(pVideoFormat->codec, NULL,
        pVideoFormat->chromaSubsampling,
        lumaBitDepth, chromaBitDepth);

#ifndef NV_RMAPI_TEGRA // TODO: Fix the Vulkan loader to support Vulkan Video on ARM
    {
        VkVideoFormatPropertiesKHR outputFormats[8];
        uint32_t outputFormatCount = sizeof(outputFormats) / sizeof(outputFormats[0]);
        memset(outputFormats, 0x00, sizeof(outputFormats));
        for (uint32_t i = 0; i < outputFormatCount; i++) {
            outputFormats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        }
        const VkVideoProfilesKHR videoProfiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILES_KHR, NULL, 1, videoProfile.GetProfile() };
        const VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, NULL,
            (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR),
            &videoProfiles };

        result = vk::GetPhysicalDeviceVideoFormatPropertiesKHR(m_pVulkanDecodeContext.physicalDev, &videoFormatInfo, &outputFormatCount, outputFormats);
        assert(result == VK_SUCCESS);
        assert(outputFormatCount);
        assert(outputFormatCount <= sizeof(outputFormats) / sizeof(outputFormats[0]));
        result = vk::GetPhysicalDeviceVideoFormatPropertiesKHR(m_pVulkanDecodeContext.physicalDev, &videoFormatInfo, &outputFormatCount, outputFormats);
        assert(result == VK_SUCCESS);
        if (m_dumpDecodeData)
            std::cout << "\t\t\tH265 formats: " << std::endl;
        for (unsigned fmt = 0; fmt < outputFormatCount; fmt++) {
            if (m_dumpDecodeData)
                std::cout << "\t\t\t" << outputFormats[fmt].format;
            if (m_dumpDecodeData)
                std::cout << std::endl;
        }

        VkVideoCapabilitiesKHR videoDecodeCapabilities = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, NULL };
        vk::GetPhysicalDeviceVideoCapabilitiesKHR(m_pVulkanDecodeContext.physicalDev,
            videoProfile.GetProfile(),
            &videoDecodeCapabilities);
    }
#endif // TODO: Fix the Vulkan loader to support Vulkan Video on ARM

    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_SPEC_VERSION };
    static const VkExtensionProperties h265StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H265_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_SPEC_VERSION };

    VkVideoDecodeH264SessionCreateInfoEXT createInfoH264 = VkVideoDecodeH264SessionCreateInfoEXT();
    createInfoH264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_CREATE_INFO_EXT;
    createInfoH264.pStdExtensionVersion = &h264StdExtensionVersion;

    VkVideoDecodeH265SessionCreateInfoEXT createInfoH265 = VkVideoDecodeH265SessionCreateInfoEXT();
    createInfoH265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_CREATE_INFO_EXT;
    createInfoH265.pStdExtensionVersion = &h265StdExtensionVersion;

    VkVideoSessionCreateInfoKHR createInfo = VkVideoSessionCreateInfoKHR();
    createInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    createInfo.flags = 0;
    createInfo.pVideoProfile = videoProfile.GetProfile();
    // createInfo.queueFamilyIndex = VK_QUEUE_VIDEO_DECODE_BIT_KHR /* FIXME in vulkan based on the enumeration */;
    createInfo.maxCodedExtent = { pVideoFormat->coded_width, pVideoFormat->coded_height };
    createInfo.maxReferencePicturesSlotsCount = maxDpbSlotCount;
    createInfo.maxReferencePicturesActiveCount = maxDpbSlotCount;
    // FIXME: For interlaced format, we need at least two layers: for top/bottom field.
    // createInfo.maxReferenceLayersCount requires NVIDIA specific extension VK_KHR_video_layers, not enabled, just yet.
    // createInfo.maxReferenceLayersCount = pVideoFormat->progressive_sequence ? 1 : 2 /* for top/bottom field */;
    createInfo.referencePicturesFormat = CodecGetVkFormat(pVideoFormat->chromaSubsampling, pVideoFormat->bit_depth_luma_minus8,
        pVideoFormat->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR /* isSemiPlanar */);

    createInfo.pictureFormat = createInfo.referencePicturesFormat;

    switch ((int32_t)videoProfile.GetCodecType()) {
    case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
        createInfo.pNext = &createInfoH264;
        break;
    case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
        createInfo.pNext = &createInfoH265;
        break;
    default:
        assert(0);
    }
    result = vk::CreateVideoSessionKHR(m_pVulkanDecodeContext.dev, &createInfo, NULL, &m_vkVideoDecoder);
    assert(result == VK_SUCCESS);

    const uint32_t maxMemReq = 8;
    uint32_t decodeSessionMemoryRequirementsCount = 0;
    VkMemoryRequirements2 memoryRequirements[maxMemReq];
    VkVideoGetMemoryPropertiesKHR decodeSessionMemoryRequirements[maxMemReq];
    // Get the count first
    result = vk::GetVideoSessionMemoryRequirementsKHR(m_pVulkanDecodeContext.dev, m_vkVideoDecoder,
        &decodeSessionMemoryRequirementsCount, NULL);
    assert(result == VK_SUCCESS);
    assert(decodeSessionMemoryRequirementsCount <= maxMemReq);

    memset(decodeSessionMemoryRequirements, 0x00, sizeof(decodeSessionMemoryRequirements));
    memset(memoryRequirements, 0x00, sizeof(memoryRequirements));
    for (uint32_t i = 0; i < decodeSessionMemoryRequirementsCount; i++) {
        decodeSessionMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_GET_MEMORY_PROPERTIES_KHR;
        decodeSessionMemoryRequirements[i].pMemoryRequirements = &memoryRequirements[i];
        memoryRequirements[i].sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    }

    result = vk::GetVideoSessionMemoryRequirementsKHR(m_pVulkanDecodeContext.dev, m_vkVideoDecoder,
        &decodeSessionMemoryRequirementsCount, decodeSessionMemoryRequirements);
    assert(result == VK_SUCCESS);

    uint32_t decodeSessionBindMemoryCount = decodeSessionMemoryRequirementsCount;
    VkVideoBindMemoryKHR decodeSessionBindMemory[8];

    vulkanVideoUtils::VulkanDeviceInfo videoRendererDeviceInfo(m_pVulkanDecodeContext.instance, m_pVulkanDecodeContext.physicalDev, m_pVulkanDecodeContext.dev);

    for (unsigned memIdx = 0; memIdx < decodeSessionBindMemoryCount; memIdx++) {
        result = memoryDecoderBound[memIdx].AllocMemory(&videoRendererDeviceInfo, &memoryRequirements[memIdx].memoryRequirements,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        assert(result == VK_SUCCESS);
        decodeSessionBindMemory[memIdx].pNext = NULL;
        decodeSessionBindMemory[memIdx].sType = VK_STRUCTURE_TYPE_VIDEO_BIND_MEMORY_KHR;
        decodeSessionBindMemory[memIdx].memory = memoryDecoderBound[memIdx].memory;

        decodeSessionBindMemory[memIdx].memoryBindIndex = decodeSessionMemoryRequirements[memIdx].memoryBindIndex;
        decodeSessionBindMemory[memIdx].memoryOffset = 0;
        decodeSessionBindMemory[memIdx].memorySize = memoryRequirements[memIdx].memoryRequirements.size;
    }

    result = vk::BindVideoSessionMemoryKHR(m_pVulkanDecodeContext.dev, m_vkVideoDecoder, decodeSessionBindMemoryCount,
        decodeSessionBindMemory);
    assert(result == VK_SUCCESS);

    VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = videoProfile.GetProfile();
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = createInfo.referencePicturesFormat;
    imageCreateInfo.extent = { m_width, m_height, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.queueFamilyIndexCount = 1;
    imageCreateInfo.pQueueFamilyIndices = &m_pVulkanDecodeContext.videoDecodeQueueFamily;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.flags = 0;
    // assert(m_numDecodeSurfaces <= 17);
    m_pVideoFrameBuffer->InitImagePool(m_numDecodeSurfaces, &imageCreateInfo, videoProfile.GetProfile());

    std::cout << "Allocating Video Device Memory" << std::endl
              << "Allocating " << m_numDecodeSurfaces << " Num Decode Surfaces and " << maxDpbSlotCount << " Video Device Memory Images for DPB " << std::endl
              << m_surfaceWidth << " x " << m_surfaceHeight << std::endl;

    m_maxDecodeFramesCount = m_numDecodeSurfaces;
    m_decodeFramesData = new NvVkDecodeFrameData[m_maxDecodeFramesCount];

    const VkDeviceSize bufferSize = ((pVideoFormat->coded_width > 3840) ? 8 : 4) * 1024 * 1024 /* 4MB or 8MB each for 8k use case */;
    const VkDeviceSize bufferOffsetAlignment = 256;
    const VkDeviceSize bufferSizeAlignment = 256;
    for (uint32_t decodeFrameId = 0; decodeFrameId < m_maxDecodeFramesCount; decodeFrameId++) {
        result = m_decodeFramesData[decodeFrameId].bistreamBuffer.CreateVideoBistreamBuffer(
            m_pVulkanDecodeContext.physicalDev, m_pVulkanDecodeContext.dev, m_pVulkanDecodeContext.videoDecodeQueueFamily,
            bufferSize, bufferOffsetAlignment, bufferSizeAlignment);
        assert(result == VK_SUCCESS);
    }

    VkCommandPoolCreateInfo cmdPoolInfo = {};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmdPoolInfo.queueFamilyIndex = m_pVulkanDecodeContext.videoDecodeQueueFamily;
    result = vk::CreateCommandPool(m_pVulkanDecodeContext.dev, &cmdPoolInfo, nullptr, &m_videoCommandPool);
    assert(result == VK_SUCCESS);

    VkCommandBufferAllocateInfo cmdInfo = {};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandBufferCount = m_maxDecodeFramesCount;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandPool = m_videoCommandPool;
    VkCommandBuffer* commandBuffers = new VkCommandBuffer[m_maxDecodeFramesCount];
    memset(commandBuffers, 0, m_maxDecodeFramesCount * sizeof(VkCommandBuffer));
    result = vk::AllocateCommandBuffers(m_pVulkanDecodeContext.dev, &cmdInfo, commandBuffers);
    assert(result == VK_SUCCESS);

    for (uint32_t decodeFrameId = 0; decodeFrameId < cmdInfo.commandBufferCount; decodeFrameId++) {
        m_decodeFramesData[decodeFrameId].commandBuffer = commandBuffers[decodeFrameId];
    }
    delete[] commandBuffers;

    return m_numDecodeSurfaces;
}

/* Callback function to be registered for getting a callback when a decoded
 * frame is ready to be decoded. Return value from HandlePictureDecode() are
 * interpreted as: 0: fail, >=1: suceeded
 */
int NvVkDecoder::DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo)
{
    if (!m_vkVideoDecoder) {
        assert(!"Decoder not initialized!");
        return -1;
    }

    int32_t currPicIdx = pPicParams->currPicIdx;
    assert((uint32_t)currPicIdx < m_numDecodeSurfaces);

    int32_t picNumInDecodeOrder = m_decodePicCount++;
    m_pVideoFrameBuffer->SetPicNumInDecodeOrder(currPicIdx, picNumInDecodeOrder);

    NvVkDecodeFrameData* pFrameData = GetCurrentFrameData((uint32_t)currPicIdx);

    assert(pFrameData->bistreamBuffer.GetBufferSize() >= pPicParams->bitstreamDataLen);

    VkDeviceSize dstBufferOffset = 0;
    pFrameData->bistreamBuffer.CopyVideoBistreamToBuffer(pPicParams->pBitstreamData, pPicParams->bitstreamDataLen, dstBufferOffset);

    pPicParams->decodeFrameInfo.srcBuffer = pFrameData->bistreamBuffer.get();
    pPicParams->decodeFrameInfo.srcBufferOffset = 0;
    pPicParams->decodeFrameInfo.srcBufferRange = GPU_ALIGN(pPicParams->bitstreamDataLen);
    // pPicParams->decodeFrameInfo.dstImageView = VkImageView();
    pPicParams->decodeFrameInfo.codedExtent = { m_width, m_height };

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = NULL;

    vk::BeginCommandBuffer(pFrameData->commandBuffer, &beginInfo);
    VkVideoBeginCodingInfoKHR decodeBeginInfo = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    // CmdResetQueryPool are NOT Supported yet.

    decodeBeginInfo.codecQualityPreset = VK_VIDEO_CODING_QUALITY_PRESET_NORMAL_BIT_KHR;
    decodeBeginInfo.videoSession = m_vkVideoDecoder;

    VulkanVideoFrameBuffer::PictureResourceInfo currentPictureResource = VulkanVideoFrameBuffer::PictureResourceInfo();
    int8_t setupReferencePictureIndex = (int8_t)pPicParams->currPicIdx;
    if (1 != m_pVideoFrameBuffer->GetImageResourcesByIndex(1, &setupReferencePictureIndex, &pPicParams->decodeFrameInfo.dstPictureResource, &currentPictureResource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR)) {
        assert(!"GetImageResourcesByIndex has failed");
    }

    assert(pPicParams->decodeFrameInfo.srcBuffer);
    const VkBufferMemoryBarrier2KHR bitstreamBufferMemoryBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR,
        NULL,
        VK_PIPELINE_STAGE_2_NONE_KHR,
        VK_ACCESS_2_HOST_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
        VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR,
        VK_QUEUE_FAMILY_IGNORED,
        m_pVulkanDecodeContext.videoDecodeQueueFamily,
        pPicParams->decodeFrameInfo.srcBuffer,
        pPicParams->decodeFrameInfo.srcBufferOffset,
        pPicParams->decodeFrameInfo.srcBufferRange
    };

    static const VkImageMemoryBarrier2KHR dpbBarrierTemplates[1] = {
        { // VkImageMemoryBarrier

            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            NULL, // const void*     pNext
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR srcStageMask
            0, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask;
            VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR, // VkAccessFlags   dstAccessMask
            VK_IMAGE_LAYOUT_UNDEFINED, // VkImageLayout   oldLayout
            VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, // VkImageLayout   newLayout
            VK_QUEUE_FAMILY_IGNORED, // uint32_t        srcQueueFamilyIndex
            m_pVulkanDecodeContext.videoDecodeQueueFamily, // uint32_t   dstQueueFamilyIndex
            VkImage(), // VkImage         image;
            {
                // VkImageSubresourceRange   subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0, // uint32_t           baseMipLevel
                1, // uint32_t           levelCount
                0, // uint32_t           baseArrayLayer
                1, // uint32_t           layerCount;
            } },
    };

    VkImageMemoryBarrier2KHR imageBarriers[VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS];
    uint32_t numDpbBarriers = 0;

    if (currentPictureResource.currentImageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
        imageBarriers[numDpbBarriers].oldLayout = currentPictureResource.currentImageLayout;
        imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        imageBarriers[numDpbBarriers].image = currentPictureResource.image;
        imageBarriers[numDpbBarriers].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        assert(imageBarriers[numDpbBarriers].image);
        numDpbBarriers++;
    }

    VulkanVideoFrameBuffer::PictureResourceInfo pictureResourcesInfo[VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS];
    memset(&pictureResourcesInfo[0], 0, sizeof(pictureResourcesInfo));
    const int8_t* pGopReferenceImagesIndexes = pPicParams->pGopReferenceImagesIndexes;
    if (pPicParams->numGopReferenceSlots) {
        if (pPicParams->numGopReferenceSlots != m_pVideoFrameBuffer->GetImageResourcesByIndex(pPicParams->numGopReferenceSlots, pGopReferenceImagesIndexes, pPicParams->pictureResources, pictureResourcesInfo, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR)) {
            assert(!"GetImageResourcesByIndex has failed");
        }
        for (int32_t resId = 0; resId < pPicParams->numGopReferenceSlots; resId++) {
            // slotLayer requires NVIDIA specific extension VK_KHR_video_layers, not enabled, just yet.
            // pGopReferenceSlots[resId].slotLayerIndex = 0;
            // pictureResourcesInfo[resId].image can be a NULL handle if the picture is not-existent.
            if (pictureResourcesInfo[resId].image && (pictureResourcesInfo[resId].currentImageLayout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) && (pictureResourcesInfo[resId].currentImageLayout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR)) {
                imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
                imageBarriers[numDpbBarriers].oldLayout = pictureResourcesInfo[resId].currentImageLayout;
                imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
                imageBarriers[numDpbBarriers].image = pictureResourcesInfo[resId].image;
                assert(imageBarriers[numDpbBarriers].image);
                numDpbBarriers++;
            }
        }
    }

    decodeBeginInfo.referenceSlotCount = pPicParams->decodeFrameInfo.referenceSlotCount;
    decodeBeginInfo.pReferenceSlots = pPicParams->decodeFrameInfo.pReferenceSlots;

    if (pDecodePictureInfo->flags.unpairedField) {
        // assert(pFrameSyncinfo->frameCompleteSemaphore == VkSemaphore());
        pDecodePictureInfo->flags.syncFirstReady = true;
    }
    // FIXME the below sequence for interlaced synchronization.
    pDecodePictureInfo->flags.syncToFirstField = false;

    VulkanVideoFrameBuffer::FrameSynchronizationInfo frameSynchronizationInfo = VulkanVideoFrameBuffer::FrameSynchronizationInfo();
    frameSynchronizationInfo.hasFrameCompleteSignalFence = true;
    frameSynchronizationInfo.hasFrameCompleteSignalSemaphore = true;

    int32_t retVal = m_pVideoFrameBuffer->QueuePictureForDecode(currPicIdx, pDecodePictureInfo, &frameSynchronizationInfo);
    if (currPicIdx != retVal) {
        assert(!"QueuePictureForDecode has failed");
    }

    VkFence frameCompleteFence = frameSynchronizationInfo.frameCompleteFence;
    VkFence frameConsumerDoneFence = frameSynchronizationInfo.frameConsumerDoneFence;
    VkSemaphore frameCompleteSemaphore = frameSynchronizationInfo.frameCompleteSemaphore;
    VkSemaphore frameConsumerDoneSemaphore = frameSynchronizationInfo.frameConsumerDoneSemaphore;

    // vk::ResetQueryPool(m_vkDev, queryFrameInfo.queryPool, queryFrameInfo.query, 1);

    vk::CmdResetQueryPool(pFrameData->commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, frameSynchronizationInfo.numQueries);
    vk::CmdBeginVideoCodingKHR(pFrameData->commandBuffer, &decodeBeginInfo);

#ifndef NV_RMAPI_TEGRA // TODO: Tegra does not support layout transitions yet.
    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        VK_DEPENDENCY_BY_REGION_BIT,
        0,
        nullptr,
        1,
        &bitstreamBufferMemoryBarrier,
        numDpbBarriers,
        imageBarriers,
    };
    vk::CmdPipelineBarrier2KHR(pFrameData->commandBuffer, &dependencyInfo);
#endif // NV_RMAPI_TEGRA // TODO: Tegra does not support layout transtions yet.

    vk::CmdBeginQuery(pFrameData->commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, VkQueryControlFlags());

    vk::CmdDecodeVideoKHR(pFrameData->commandBuffer, &pPicParams->decodeFrameInfo);

    vk::CmdEndQuery(pFrameData->commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId);

    VkVideoEndCodingInfoKHR decodeEndInfo = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    vk::CmdEndVideoCodingKHR(pFrameData->commandBuffer, &decodeEndInfo);
    vk::EndCommandBuffer(pFrameData->commandBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL };
    submitInfo.waitSemaphoreCount = (frameConsumerDoneSemaphore == VkSemaphore()) ? 0 : 1;
    submitInfo.pWaitSemaphores = &frameConsumerDoneSemaphore;
    VkPipelineStageFlags videoDecodeSubmitWaitStages = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    submitInfo.pWaitDstStageMask = &videoDecodeSubmitWaitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &pFrameData->commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frameCompleteSemaphore;

    VkResult result = VK_SUCCESS;

    const uint64_t fenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;

    if ((frameConsumerDoneSemaphore == VkSemaphore()) && (frameConsumerDoneFence != VkFence())) {
        result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameConsumerDoneFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);
        result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
    }

    result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
    if (result == VK_NOT_READY) {
        std::cout << "\t *************** WARNING: frameCompleteFence is not done *************< " << currPicIdx << " >**********************" << std::endl;
        assert(!"frameCompleteFence is not signaled yet");
    }
#if 0 // For fence/sync debugging
        result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);

        result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
        if (result == VK_NOT_READY) {
            std::cout << "\t *********** WARNING: frameCompleteFence is still not done *************< " << currPicIdx << " >**********************" << std::endl;
        }
        assert(result == VK_SUCCESS);
    }
#endif

    result = vk::ResetFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence);
    assert(result == VK_SUCCESS);
    result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
    assert(result == VK_NOT_READY);

    vk::QueueSubmit(m_pVulkanDecodeContext.videoQueue, 1, &submitInfo, frameCompleteFence);

    if (m_dumpDecodeData) {
        std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
        std::cout << "\t => Decode Submitted for CurrPicIdx: " << currPicIdx << std::endl
                  << "\t\tm_nPicNumInDecodeOrder: " << picNumInDecodeOrder << "\t\tframeCompleteFence " << frameCompleteFence
                  << "\t\tframeCompleteSemaphore " << frameCompleteSemaphore << "\t\tdstImageView "
                  << pPicParams->decodeFrameInfo.dstPictureResource.imageViewBinding << std::endl;
    }

#if 0 // For fence/sync debugging
    if (frameCompleteFence == VkFence()) {
        result = vk::QueueWaitIdle(m_pVulkanDecodeContext.videoQueue);
        assert(result == VK_SUCCESS);
    } else {
        if (frameCompleteSemaphore == VkSemaphore()) {
            result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence, true, fenceTimeout);
            assert(result == VK_SUCCESS);
            result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
            assert(result == VK_SUCCESS);
        }
    }
#endif

    // For fence/sync debugging
    if (pDecodePictureInfo->flags.fieldPic) {
        result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);
        result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
        assert(result == VK_SUCCESS);
    }

    bool checkDecodeStatus = false;
    if (checkDecodeStatus) {
        struct nvVideoGetDecodeStatus {
            VkQueryResultStatusKHR decodeStatus;
            uint32_t hwCyclesCount; /**< OUT: HW cycle count per frame         */
            uint32_t hwStatus; /**< OUT: HW decode status                 */
            uint32_t mbsCorrectlyDecoded; // total numers of correctly decoded macroblocks
            uint32_t mbsInError; // number of error macroblocks.
            uint16_t instanceId; /**< OUT: nvdec instance id                */
            uint16_t reserved1; /**< Reserved for future use               */
        } decodeStatus;
        result = vk::GetQueryPoolResults(m_pVulkanDecodeContext.dev,
            frameSynchronizationInfo.queryPool,
            frameSynchronizationInfo.startQueryId,
            frameSynchronizationInfo.numQueries, sizeof(decodeStatus), &decodeStatus,
            512, VK_QUERY_RESULT_WAIT_BIT);
        assert(result == VK_SUCCESS);
        assert(decodeStatus.decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

        std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
        std::cout << "\t => Decode Status for CurrPicIdx: " << currPicIdx << std::endl
                  << "\t\tdecodeStatus: " << decodeStatus.decodeStatus << "\t\thwCyclesCount " << decodeStatus.hwCyclesCount
                  << "\t\thwStatus " << decodeStatus.hwStatus << "\t\tmbsCorrectlyDecoded " << decodeStatus.mbsCorrectlyDecoded
                  << "\t\tmbsInError " << decodeStatus.mbsInError
                  << "\t\tinstanceId " << decodeStatus.instanceId << std::endl;
    }

    return currPicIdx;
}

void NvVkDecoder::Deinitialize()
{

    if (m_pVulkanDecodeContext.videoQueue) {
        vk::QueueWaitIdle(m_pVulkanDecodeContext.videoQueue);
    }

    if (m_pVulkanDecodeContext.dev) {
        vk::DeviceWaitIdle(m_pVulkanDecodeContext.dev);
    }

    if (m_pVideoFrameBuffer) {
        m_pVideoFrameBuffer->Release();
    }

    if (m_decodeFramesData && m_videoCommandPool) {
        VkCommandBuffer* commandBuffers = new VkCommandBuffer[m_maxDecodeFramesCount];
        memset(commandBuffers, 0, m_maxDecodeFramesCount * sizeof(VkCommandBuffer));
        for (size_t decodeFrameId = 0; decodeFrameId < m_maxDecodeFramesCount; decodeFrameId++) {
            commandBuffers[decodeFrameId] = m_decodeFramesData[decodeFrameId].commandBuffer;
            assert(commandBuffers[decodeFrameId]);
            m_decodeFramesData[decodeFrameId].commandBuffer = VkCommandBuffer();
        }

        vk::FreeCommandBuffers(m_pVulkanDecodeContext.dev, m_videoCommandPool, m_maxDecodeFramesCount, commandBuffers);
        vk::DestroyCommandPool(m_pVulkanDecodeContext.dev, m_videoCommandPool, NULL);
        m_videoCommandPool = NULL;

        delete[] commandBuffers;
    }

    if (m_decodeFramesData) {
        for (size_t decodeFrameId = 0; decodeFrameId < m_maxDecodeFramesCount; decodeFrameId++) {
            m_decodeFramesData[decodeFrameId].bistreamBuffer.DestroyVideoBistreamBuffer();
        }

        delete[] m_decodeFramesData;
        m_decodeFramesData = NULL;
    }

    if (m_vkVideoDecoder) {
        vk::DestroyVideoSessionKHR(m_pVulkanDecodeContext.dev, m_vkVideoDecoder, NULL);
        m_vkVideoDecoder = VkVideoSessionKHR();
    }
}

NvVkDecoder::~NvVkDecoder()
{
    Deinitialize();
}

int32_t NvVkDecoder::AddRef()
{
    return ++m_refCount;
}

int32_t NvVkDecoder::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        Deinitialize();
        delete this;
    }
    return ret;
}
