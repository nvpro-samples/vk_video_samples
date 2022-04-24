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

#undef max
#undef min

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
        int maxLumaPS = 35651584;
        int maxDpbPicBuf = 6;
        int picSizeInSamplesY = (int)(width * height);
        int maxDpbSize;
        if (picSizeInSamplesY <= (maxLumaPS >> 2))
            maxDpbSize = maxDpbPicBuf * 4;
        else if (picSizeInSamplesY <= (maxLumaPS >> 1))
            maxDpbSize = maxDpbPicBuf * 2;
        else if (picSizeInSamplesY <= ((3 * maxLumaPS) >> 2))
            maxDpbSize = (maxDpbPicBuf * 4) / 3;
        else
            maxDpbSize = maxDpbPicBuf;
        return (std::min)(maxDpbSize, 16) + 4;
    }

    return 8;
}

VkResult NvVkDecoder::GetVideoFormats(nvVideoProfile* pVideoProfile, VkImageUsageFlags imageUsage,
                                      uint32_t& formatCount, VkFormat* formats)
{
    for (uint32_t i = 0; i < formatCount; i++) {
        formats[i] = VK_FORMAT_UNDEFINED;
    }

    const VkVideoProfilesKHR videoProfiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILES_KHR, NULL, 1, pVideoProfile->GetProfile() };
    const VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, NULL,
                                                                 imageUsage, &videoProfiles };

    uint32_t supportedFormatCount = 0;
    VkResult result = vk::GetPhysicalDeviceVideoFormatPropertiesKHR(m_pVulkanDecodeContext.physicalDev, &videoFormatInfo, &supportedFormatCount, nullptr);
    assert(result == VK_SUCCESS);
    assert(supportedFormatCount);

    VkVideoFormatPropertiesKHR* pSupportedFormats = new VkVideoFormatPropertiesKHR[supportedFormatCount];
    memset(pSupportedFormats, 0x00, supportedFormatCount * sizeof(VkVideoFormatPropertiesKHR));
    for (uint32_t i = 0; i < supportedFormatCount; i++) {
        pSupportedFormats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }

    result = vk::GetPhysicalDeviceVideoFormatPropertiesKHR(m_pVulkanDecodeContext.physicalDev, &videoFormatInfo, &supportedFormatCount, pSupportedFormats);
    assert(result == VK_SUCCESS);
    if (m_dumpDecodeData) {
        std::cout << "\t\t\t" << ((pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) ? "h264" : "h264") << "decode formats: " << std::endl;
        for (uint32_t fmt = 0; fmt < supportedFormatCount; fmt++) {
            std::cout << "\t\t\t " << fmt << ": " << std::hex << pSupportedFormats[fmt].format << std::dec << std::endl;
        }
    }

    formatCount = std::min(supportedFormatCount, formatCount);

    for (uint32_t i = 0; i < formatCount; i++) {
        formats[i] = pSupportedFormats[i].format;
    }

    delete[] pSupportedFormats;

    return result;
}

VkResult NvVkDecoder::GetVideoCapabilities(nvVideoProfile* pVideoProfile, VkVideoCapabilitiesKHR* pVideoCapabilities)
{
    assert(pVideoCapabilities->sType == VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
    VkVideoDecodeCapabilitiesKHR* pVideoDecodeCapabilities = (VkVideoDecodeCapabilitiesKHR*)pVideoCapabilities->pNext;
    assert(pVideoDecodeCapabilities->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
    VkVideoDecodeH264CapabilitiesEXT* pH264Capabilities = nullptr;
    VkVideoDecodeH265CapabilitiesEXT* pH265Capabilities = nullptr;

    if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        assert(pVideoDecodeCapabilities->pNext);
        pH264Capabilities = (VkVideoDecodeH264CapabilitiesEXT*)pVideoDecodeCapabilities->pNext;
        assert(pH264Capabilities->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_EXT);
    } else if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
        assert(pVideoDecodeCapabilities->pNext);
        pH265Capabilities = (VkVideoDecodeH265CapabilitiesEXT*)pVideoDecodeCapabilities->pNext;
        assert(pH265Capabilities->sType ==  VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_EXT);
    } else {
        assert(!"Unsupported codec");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VkResult result = vk::GetPhysicalDeviceVideoCapabilitiesKHR(m_pVulkanDecodeContext.physicalDev,
                                                                pVideoProfile->GetProfile(), pVideoCapabilities);
    assert(result == VK_SUCCESS);

    if (m_dumpDecodeData) {
        std::cout << "\t\t\t" << ((pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) ? "h264" : "h264") << "decode capabilities: " << std::endl;

        if (pVideoCapabilities->capabilityFlags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR) {
            std::cout << "\t\t\t" << "Use separate reference images" << std::endl;
        }

        std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << pVideoCapabilities->minBitstreamBufferOffsetAlignment << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << pVideoCapabilities->minBitstreamBufferSizeAlignment << std::endl;
        std::cout << "\t\t\t" << "videoPictureExtentGranularity: " << pVideoCapabilities->videoPictureExtentGranularity.width << " x " << pVideoCapabilities->videoPictureExtentGranularity.height << std::endl;
        std::cout << "\t\t\t" << "minExtent: " << pVideoCapabilities->minExtent.width << " x " << pVideoCapabilities->minExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxExtent: " << pVideoCapabilities->maxExtent.width  << " x " << pVideoCapabilities->maxExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxReferencePicturesSlotsCount: " << pVideoCapabilities->maxReferencePicturesSlotsCount << std::endl;
        std::cout << "\t\t\t" << "maxReferencePicturesActiveCount: " << pVideoCapabilities->maxReferencePicturesActiveCount << std::endl;

        if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
            std::cout << "\t\t\t" << "maxLevel: " << pH264Capabilities->maxLevel << std::endl;
            std::cout << "\t\t\t" << "fieldOffsetGranularity: " << pH264Capabilities->fieldOffsetGranularity.x << " x " << pH264Capabilities->fieldOffsetGranularity.y << std::endl;;

            if (strncmp(pVideoCapabilities->stdHeaderVersion.extensionName,
                    VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
                        sizeof (pVideoCapabilities->stdHeaderVersion.extensionName) - 1U) ||
                (pVideoCapabilities->stdHeaderVersion.specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION)) {
                assert(!"Unsupported h.264 STD version");
                return VK_ERROR_INCOMPATIBLE_DRIVER;
            }
        } else if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
            std::cout << "\t\t\t" << "maxLevel: " << pH265Capabilities->maxLevel << std::endl;
            if (strncmp(pVideoCapabilities->stdHeaderVersion.extensionName,
                    VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME,
                        sizeof (pVideoCapabilities->stdHeaderVersion.extensionName) - 1U) ||
                (pVideoCapabilities->stdHeaderVersion.specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION)) {
                assert(!"Unsupported h.265 STD version");
                return VK_ERROR_INCOMPATIBLE_DRIVER;
            }
        } else {
            assert(!"Unsupported codec");
        }
    }

    return result;
}

/* Callback function to be registered for getting a callback when decoding of
 * sequence starts. Return value from HandleVideoSequence() are interpreted as :
 *  0: fail, 1: suceeded, > 1: override dpb size of parser (set by
 * nvVideoParseParameters::ulMaxNumDecodeSurfaces while creating parser)
 */
int32_t NvVkDecoder::StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat)
{
    const bool testUseLargestSurfaceExtent = false;
    // Assume 4k content for testing surfaces
    const uint32_t surfaceMinWidthExtent  = 3840;
    const uint32_t surfaceMinHeightExtent = 2160;

    VkExtent2D codedExtent = { pVideoFormat->coded_width, pVideoFormat->coded_height };

    // Width and height of the image surface
    VkExtent2D imageExtent = VkExtent2D { (uint32_t)(pVideoFormat->display_area.right - pVideoFormat->display_area.left),
                                          (uint32_t)(pVideoFormat->display_area.bottom - pVideoFormat->display_area.top) };

    // If we are testing content with different sizes against max sized surface vs. images dynamic resize
    // then set the imageExtent to the max surface size selected.
    if (testUseLargestSurfaceExtent) {
        imageExtent = { std::max(surfaceMinWidthExtent,  imageExtent.width),
                        std::max(surfaceMinHeightExtent, imageExtent.height) };
    }

    std::cout << "Video Input Information" << std::endl
              << "\tCodec        : " << GetVideoCodecString(pVideoFormat->codec) << std::endl
              << "\tFrame rate   : " << pVideoFormat->frame_rate.numerator << "/" << pVideoFormat->frame_rate.denominator << " = "
              << 1.0 * pVideoFormat->frame_rate.numerator / pVideoFormat->frame_rate.denominator << " fps" << std::endl
              << "\tSequence     : " << (pVideoFormat->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
              << "\tCoded size   : [" << codedExtent.width << ", " << codedExtent.height << "]" << std::endl
              << "\tDisplay area : [" << pVideoFormat->display_area.left << ", " << pVideoFormat->display_area.top << ", "
              << pVideoFormat->display_area.right << ", " << pVideoFormat->display_area.bottom << "]" << std::endl
              << "\tChroma       : " << GetVideoChromaFormatString(pVideoFormat->chromaSubsampling) << std::endl
              << "\tBit depth    : " << pVideoFormat->bit_depth_luma_minus8 + 8 << std::endl;

    m_numDecodeSurfaces = std::max(m_numDecodeSurfaces, GetNumDecodeSurfaces(pVideoFormat->codec, pVideoFormat->minNumDecodeSurfaces, codedExtent.width, codedExtent.height));

    VkResult result = VK_SUCCESS;

    VkVideoCodecOperationFlagsKHR videoCodecs = vk::GetSupportedCodecs(m_pVulkanDecodeContext.physicalDev,
        (int32_t*)&m_pVulkanDecodeContext.videoDecodeQueueFamily,
        VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT);
    assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_INVALID_BIT_KHR);

    if (m_dumpDecodeData) {
        std::cout << "\t" << std::hex << videoCodecs << " HW codec types are available: " << std::dec << std::endl;
    }

    VkVideoCodecOperationFlagBitsKHR videoCodec = pVideoFormat->codec;

    if (m_dumpDecodeData) {
        std::cout << "\tcodec " << nvVideoProfile::CodecToName(videoCodec) << std::endl;
    }

    nvVideoProfile videoProfile(videoCodec, pVideoFormat->chromaSubsampling, pVideoFormat->lumaBitDepth, pVideoFormat->chromaBitDepth);
    if (!IsCodecTypeSupported(m_pVulkanDecodeContext.physicalDev, m_pVulkanDecodeContext.videoDecodeQueueFamily, videoCodec)) {
        std::cout << "*** The video codec " << nvVideoProfile::CodecToName(videoCodec) << " is not supported! ***" << std::endl;
        assert("The video codec is not supported");
        return -1;
    }

    if (m_videoFormat.coded_width && m_videoFormat.coded_height) {
        // CreateDecoder() has been called before, and now there's possible config change
        if (m_pVulkanDecodeContext.videoQueue) {
            vk::QueueWaitIdle(m_pVulkanDecodeContext.videoQueue);
        }

        if (m_pVulkanDecodeContext.dev) {
            vk::DeviceWaitIdle(m_pVulkanDecodeContext.dev);
        }
    }

    std::cout << "Video Decoding Params:" << std::endl
              << "\tNum Surfaces : " << m_numDecodeSurfaces << std::endl
              << "\tResize       : " << codedExtent.width << " x " << codedExtent.height << std::endl;

    uint32_t maxDpbSlotCount = pVideoFormat->maxNumDpbSlots; // This is currently configured by the parser to maxNumDpbSlots from the stream plus 1 for the current slot on the fly

    assert(VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR == pVideoFormat->chromaSubsampling);


    VkFormat supportedDpbAndOutFormats[8];
    uint32_t formatCount = sizeof(supportedDpbAndOutFormats) / sizeof(supportedDpbAndOutFormats[0]);
    result = GetVideoFormats(&videoProfile,
                             (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR),
                              formatCount, supportedDpbAndOutFormats);

    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: GetVideoFormats() result: 0x%x\n", result);
    }

    VkVideoDecodeCapabilitiesKHR videoDecodeCapabilities = { VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, NULL };
    VkVideoCapabilitiesKHR videoCapabilities             = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &videoDecodeCapabilities };
    VkVideoDecodeH264CapabilitiesEXT h264Capabilities    = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_EXT, NULL };
    VkVideoDecodeH265CapabilitiesEXT h265Capabilities    = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_EXT, NULL };
    if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        videoDecodeCapabilities.pNext = &h264Capabilities;
    } else if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
        videoDecodeCapabilities.pNext = &h265Capabilities;
    } else {
        assert(!"Unsupported codec");
        return -1;
    }
    result = GetVideoCapabilities(&videoProfile, &videoCapabilities);
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: GetVideoCapabilities() result: 0x%x\n", result);
    }

    const VkVideoProfileKHR* pVideoProfile = videoProfile.GetProfile();
    VkFormat referencePicturesFormat = videoProfile.CodecGetVkFormat(
                 (VkVideoChromaSubsamplingFlagBitsKHR)pVideoProfile->chromaSubsampling,
                 (VkVideoComponentBitDepthFlagBitsKHR)pVideoProfile->lumaBitDepth,
                 pVideoProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR /* isSemiPlanar */);

    VkFormat pictureFormat = referencePicturesFormat;
    assert(supportedDpbAndOutFormats[0] == pictureFormat);

    m_capabilityFlags  = (VkVideoDecodeCapabilityFlagBitsKHR)videoDecodeCapabilities.flags;
    imageExtent.width  = std::max(imageExtent.width, videoCapabilities.minExtent.width);
    imageExtent.height = std::max(imageExtent.height, videoCapabilities.minExtent.height);

    uint32_t alignWidth = videoCapabilities.videoPictureExtentGranularity.width - 1;
    imageExtent.width = ((imageExtent.width + alignWidth) & ~alignWidth);
    uint32_t alignHeight = videoCapabilities.videoPictureExtentGranularity.height - 1;
    imageExtent.height = ((imageExtent.height + alignHeight) & ~alignHeight);

    if (!m_videoSession ||
            !m_videoSession->IsCompatible( m_pVulkanDecodeContext.dev,
                                           m_pVulkanDecodeContext.videoDecodeQueueFamily,
                                           &videoProfile,
                                           pictureFormat,
                                           imageExtent,
                                           referencePicturesFormat,
                                           maxDpbSlotCount,
                                           maxDpbSlotCount) ) {
        result = NvVideoSession::Create( m_pVulkanDecodeContext.dev,
                                         m_pVulkanDecodeContext.videoDecodeQueueFamily,
                                         &videoProfile,
                                         pictureFormat,
                                         imageExtent,
                                         referencePicturesFormat,
                                         maxDpbSlotCount,
                                         maxDpbSlotCount,
                                         m_videoSession);

        // after creating a new video session, we need codec reset.
        m_resetDecoder = true;
        assert(result == VK_SUCCESS);
    }

    int32_t ret =
    m_pVideoFrameBuffer->InitImagePool(videoProfile.GetProfile(),
                                       m_numDecodeSurfaces,
                                       referencePicturesFormat,
                                       codedExtent,
                                       imageExtent,
                                       VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                                            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                                       m_pVulkanDecodeContext.videoDecodeQueueFamily);

    assert((uint32_t)ret == m_numDecodeSurfaces);
    if ((uint32_t)ret != m_numDecodeSurfaces) {
        fprintf(stderr, "\nERROR: InitImagePool() ret(%d) != m_numDecodeSurfaces(%d)\n", ret, m_numDecodeSurfaces);
    }

    std::cout << "Allocating Video Device Memory" << std::endl
              << "Allocating " << m_numDecodeSurfaces << " Num Decode Surfaces and " << maxDpbSlotCount << " Video Device Memory Images for DPB " << std::endl
              << imageExtent.width << " x " << imageExtent.height << std::endl;

    m_maxDecodeFramesCount = m_numDecodeSurfaces;

    m_decodeFramesData.resize(m_maxDecodeFramesCount, std::max(surfaceMinWidthExtent, imageExtent.width),
                              videoCapabilities.minBitstreamBufferOffsetAlignment,
                              videoCapabilities.minBitstreamBufferSizeAlignment);


    // Save the original config
    m_videoFormat = *pVideoFormat;

    return m_numDecodeSurfaces;
}

bool NvVkDecoder::UpdatePictureParameters(VkPictureParameters* pPictureParameters,
                                          VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
                                          uint64_t updateSequenceCount)
{

    VkSharedBaseObj<StdVideoPictureParametersSet> pictureParametersSet(StdVideoPictureParametersSet::Create(pPictureParameters, updateSequenceCount));
    if (!pictureParametersSet) {
        assert(!"Invalid pictureParametersSet");
        return false;
    }

    bool hasSpsPpsPair = false;
    uint32_t numEntires = AddPictureParametersToQueue(pictureParametersSet, hasSpsPpsPair);

    if (m_videoSession && hasSpsPpsPair && numEntires) {
        FlushPictureParametersQueue();
    }

    pictureParametersObject = pictureParametersSet;
    return true;
}

uint32_t NvVkDecoder::AddPictureParametersToQueue(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet, bool& hasSpsPpsPair)
{

    hasSpsPpsPair = false;

    if (!m_pictureParametersQueue.empty()) {
        m_pictureParametersQueue.push(pictureParametersSet);
        return (uint32_t)m_pictureParametersQueue.size();
    }

    bool isSps = false;
    int32_t spsId = pictureParametersSet->GetSpsId(isSps);

    // Attempt to combine the pair of SPS/PPS to avid creatingPicture Parameter Objects
    if ((!!m_lastSpsPictureParametersQueue && !!m_lastPpsPictureParametersQueue) || // the last slots are already occupied
            (isSps && !!m_lastSpsPictureParametersQueue) ||  // the current one is SPS but SPS slot is already occupied
            (!isSps && !!m_lastPpsPictureParametersQueue) || // the current one is PPS but PPS slot is already occupied
            ((m_lastSpsIdInQueue != -1) && (m_lastSpsIdInQueue != spsId) )) { // This has a different spsId

        if (m_lastSpsPictureParametersQueue) {
            m_pictureParametersQueue.push(m_lastSpsPictureParametersQueue);
            m_lastSpsPictureParametersQueue = NULL;
        }

        if (m_lastPpsPictureParametersQueue) {
            m_pictureParametersQueue.push(m_lastPpsPictureParametersQueue);
            m_lastPpsPictureParametersQueue = NULL;
        }

        m_pictureParametersQueue.push(pictureParametersSet);

        m_lastSpsIdInQueue = -1;
        return (uint32_t)m_pictureParametersQueue.size();
    }

    if (m_lastSpsIdInQueue == -1) {
        m_lastSpsIdInQueue = spsId;
    }

    assert(m_lastSpsIdInQueue != -1);
    if (isSps) {
        m_lastSpsPictureParametersQueue = pictureParametersSet;
    } else {
        m_lastPpsPictureParametersQueue = pictureParametersSet;
    }

    uint32_t count = 0;
    if (m_lastSpsPictureParametersQueue) {
        count++;
    }

    if (m_lastPpsPictureParametersQueue) {
        count++;
    }

    hasSpsPpsPair = (count == 2);

    return count;
}

uint32_t NvVkDecoder::FlushPictureParametersQueue()
{
    uint32_t numQueueItems = 0;
    while (!m_pictureParametersQueue.empty()) {
        VkSharedBaseObj<StdVideoPictureParametersSet>& ppItem = m_pictureParametersQueue.front();

        bool isSps = false;
        ppItem->GetSpsId(isSps);

        VkSharedBaseObj<StdVideoPictureParametersSet> emptyStdPictureParametersSet;

        AddPictureParameters(isSps ? ppItem : emptyStdPictureParametersSet,
                             isSps ?  emptyStdPictureParametersSet : ppItem);

        m_pictureParametersQueue.pop();
        numQueueItems++;
    }

    if (numQueueItems) {
        return numQueueItems;
    }

    if (!(m_lastSpsPictureParametersQueue || m_lastPpsPictureParametersQueue)) {
        return 0;
    }

    AddPictureParameters(m_lastSpsPictureParametersQueue,
                         m_lastPpsPictureParametersQueue);

    if (m_lastSpsPictureParametersQueue) {
        numQueueItems++;
        m_lastSpsPictureParametersQueue = NULL;
    }

    if (m_lastPpsPictureParametersQueue) {
        numQueueItems++;
        m_lastPpsPictureParametersQueue = NULL;
    }

    m_lastSpsIdInQueue = -1;

    assert(numQueueItems);

    return numQueueItems;
}

bool NvVkDecoder::CheckStdObjectBeforeUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet)
{
    if (!stdPictureParametersSet) {
        return false;
    }

    bool stdObjectUpdate = (stdPictureParametersSet->m_updateSequenceCount > 0);

    if (!m_currentPictureParameters || stdObjectUpdate) {

        assert(m_videoSession);
        assert(stdObjectUpdate || (!stdPictureParametersSet->m_videoSession));
        // Create new Vulkan Picture Parameters object
        return true;

    } else { // new std object
        assert(!stdPictureParametersSet->m_vkObjectOwner);
        assert(!stdPictureParametersSet->m_videoSession);
        assert(m_currentPictureParameters);
        // Update the existing Vulkan Picture Parameters object
    }

    return false;
}

VkParserVideoPictureParameters* NvVkDecoder::CheckStdObjectAfterUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet, VkParserVideoPictureParameters* pNewPictureParametersObject)
{
    if (!stdPictureParametersSet) {
        return NULL;
    }

    if (pNewPictureParametersObject) {
        if (stdPictureParametersSet->m_updateSequenceCount == 0) {
            stdPictureParametersSet->m_videoSession = m_videoSession;
        } else {
            const VkParserVideoPictureParameters* pOwnerPictureParameters =
                    VkParserVideoPictureParameters::VideoPictureParametersFromBase(stdPictureParametersSet->m_vkObjectOwner);
            if (pOwnerPictureParameters) {
                assert(pOwnerPictureParameters->GetId() < pNewPictureParametersObject->GetId());
            }
        }
        // new object owner
        stdPictureParametersSet->m_vkObjectOwner = pNewPictureParametersObject;
        return pNewPictureParametersObject;

    } else { // new std object
        stdPictureParametersSet->m_videoSession = m_videoSession;
        stdPictureParametersSet->m_vkObjectOwner = m_currentPictureParameters;
    }

    return m_currentPictureParameters;
}

VkParserVideoPictureParameters*  NvVkDecoder::AddPictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& spsStdPictureParametersSet,
                                                                   VkSharedBaseObj<StdVideoPictureParametersSet>& ppsStdPictureParametersSet)
{

    VkParserVideoPictureParameters* pPictureParametersObject = NULL;
    bool createNewObject = CheckStdObjectBeforeUpdate(spsStdPictureParametersSet);
    createNewObject |= CheckStdObjectBeforeUpdate(ppsStdPictureParametersSet);

    if (createNewObject) {
        pPictureParametersObject = VkParserVideoPictureParameters::Create(m_pVulkanDecodeContext.dev,
                                                                          m_videoSession,
                                                                          spsStdPictureParametersSet,
                                                                          ppsStdPictureParametersSet,
                                                                          m_currentPictureParameters);
        m_currentPictureParameters = pPictureParametersObject;
    } else {
        m_currentPictureParameters->Update(spsStdPictureParametersSet, ppsStdPictureParametersSet);
    }

    CheckStdObjectAfterUpdate(spsStdPictureParametersSet, pPictureParametersObject);
    CheckStdObjectAfterUpdate(ppsStdPictureParametersSet, pPictureParametersObject);

    return pPictureParametersObject;
}

/* Callback function to be registered for getting a callback when a decoded
 * frame is ready to be decoded. Return value from HandlePictureDecode() are
 * interpreted as: 0: fail, >=1: suceeded
 */
int NvVkDecoder::DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo)
{
    if (!m_videoSession) {
        assert(!"Decoder not initialized!");
        return -1;
    }

    int32_t currPicIdx = pPicParams->currPicIdx;
    assert((uint32_t)currPicIdx < m_numDecodeSurfaces);

    int32_t picNumInDecodeOrder = m_decodePicCount++;
    m_pVideoFrameBuffer->SetPicNumInDecodeOrder(currPicIdx, picNumInDecodeOrder);

    NvVkDecodeFrameDataSlot frameDataSlot;
    int32_t retPicIdx = GetCurrentFrameData((uint32_t)currPicIdx, frameDataSlot);
    assert(retPicIdx == currPicIdx);

    if (retPicIdx != currPicIdx) {
        fprintf(stderr, "\nERROR: DecodePictureWithParameters() retPicIdx(%d) != currPicIdx(%d)\n", retPicIdx, currPicIdx);
    }

    assert(frameDataSlot.bitstreamBuffer->GetBufferSize() >= pPicParams->bitstreamDataLen);

    VkDeviceSize dstBufferOffset = 0;
    frameDataSlot.bitstreamBuffer->CopyVideoBitstreamToBuffer(pPicParams->pBitstreamData, pPicParams->bitstreamDataLen, dstBufferOffset);

    pPicParams->decodeFrameInfo.srcBuffer = frameDataSlot.bitstreamBuffer->get();
    pPicParams->decodeFrameInfo.srcBufferOffset = 0;
    pPicParams->decodeFrameInfo.srcBufferRange = GPU_ALIGN(pPicParams->bitstreamDataLen);
    // pPicParams->decodeFrameInfo.dstImageView = VkImageView();

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = NULL;

    vk::BeginCommandBuffer(frameDataSlot.commandBuffer, &beginInfo);
    VkVideoBeginCodingInfoKHR decodeBeginInfo = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    // CmdResetQueryPool are NOT Supported yet.

    decodeBeginInfo.codecQualityPreset = VK_VIDEO_CODING_QUALITY_PRESET_NORMAL_BIT_KHR;
    decodeBeginInfo.videoSession = m_videoSession->GetVideoSession();

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

    FlushPictureParametersQueue();

    assert(pPicParams->pCurrentPictureParameters->m_vkObjectOwner);
    const VkParserVideoPictureParameters* pOwnerPictureParameters =
            VkParserVideoPictureParameters::VideoPictureParametersFromBase(pPicParams->pCurrentPictureParameters->m_vkObjectOwner);
    assert(pOwnerPictureParameters);
    assert(pOwnerPictureParameters->GetId() <= m_currentPictureParameters->GetId());

    bool isSps = false;
    int32_t spsId = pPicParams->pCurrentPictureParameters->GetSpsId(isSps);
    assert(!isSps);
    assert(spsId >= 0);
    assert(pOwnerPictureParameters->HasSpsId(spsId));
    bool isPps = false;
    int32_t ppsId =  pPicParams->pCurrentPictureParameters->GetPpsId(isPps);
    assert(isPps);
    assert(ppsId >= 0);
    assert(pOwnerPictureParameters->HasPpsId(ppsId));

    decodeBeginInfo.videoSessionParameters = *pOwnerPictureParameters;

    if (m_dumpDecodeData) {
        std::cout << "Using object " << decodeBeginInfo.videoSessionParameters << " with ID: (" << pOwnerPictureParameters->GetId() << ")" << " for SPS: " <<  spsId << ", PPS: " << ppsId << std::endl;
    }

    int32_t retVal = m_pVideoFrameBuffer->QueuePictureForDecode(currPicIdx, pDecodePictureInfo, pPicParams->pCurrentPictureParameters->m_vkObjectOwner, &frameSynchronizationInfo);
    if (currPicIdx != retVal) {
        assert(!"QueuePictureForDecode has failed");
    }

    VkFence frameCompleteFence = frameSynchronizationInfo.frameCompleteFence;
    VkFence frameConsumerDoneFence = frameSynchronizationInfo.frameConsumerDoneFence;
    VkSemaphore frameCompleteSemaphore = frameSynchronizationInfo.frameCompleteSemaphore;
    VkSemaphore frameConsumerDoneSemaphore = frameSynchronizationInfo.frameConsumerDoneSemaphore;

    // vk::ResetQueryPool(m_vkDev, queryFrameInfo.queryPool, queryFrameInfo.query, 1);

    vk::CmdResetQueryPool(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, frameSynchronizationInfo.numQueries);
    vk::CmdBeginVideoCodingKHR(frameDataSlot.commandBuffer, &decodeBeginInfo);

    if (m_resetDecoder) {
        VkVideoCodingControlInfoKHR codingControlInfo = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
                                                          nullptr,
                                                          VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR };

        // Video spec requires mandatory codec reset before the first frame.
        vk::CmdControlVideoCodingKHR(frameDataSlot.commandBuffer, &codingControlInfo);
        // Done with the reset
        m_resetDecoder = false;
    }

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
    vk::CmdPipelineBarrier2KHR(frameDataSlot.commandBuffer, &dependencyInfo);

    vk::CmdBeginQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, VkQueryControlFlags());

    vk::CmdDecodeVideoKHR(frameDataSlot.commandBuffer, &pPicParams->decodeFrameInfo);

    vk::CmdEndQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId);

    VkVideoEndCodingInfoKHR decodeEndInfo = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    vk::CmdEndVideoCodingKHR(frameDataSlot.commandBuffer, &decodeEndInfo);
    vk::EndCommandBuffer(frameDataSlot.commandBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL };
    submitInfo.waitSemaphoreCount = (frameConsumerDoneSemaphore == VkSemaphore()) ? 0 : 1;
    submitInfo.pWaitSemaphores = &frameConsumerDoneSemaphore;
    VkPipelineStageFlags videoDecodeSubmitWaitStages = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    submitInfo.pWaitDstStageMask = &videoDecodeSubmitWaitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frameDataSlot.commandBuffer;
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

    const bool checkDecodeFences = false; // For decoder fences debugging
    if (checkDecodeFences) { // For fence/sync debugging
        result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);

        result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
        if (result == VK_NOT_READY) {
            std::cout << "\t *********** WARNING: frameCompleteFence is still not done *************< " << currPicIdx << " >**********************" << std::endl;
        }
        assert(result == VK_SUCCESS);
    }

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

    const bool checkDecodeIdleSync = false; // For fence/sync/idle debugging
    if (checkDecodeIdleSync) { // For fence/sync debugging
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
    }

    // For fence/sync debugging
    if (pDecodePictureInfo->flags.fieldPic) {
        result = vk::WaitForFences(m_pVulkanDecodeContext.dev, 1, &frameCompleteFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);
        result = vk::GetFenceStatus(m_pVulkanDecodeContext.dev, frameCompleteFence);
        assert(result == VK_SUCCESS);
    }

    const bool checkDecodeStatus = false; // Check the queries
    if (checkDecodeStatus) {

        VkQueryResultStatusKHR decodeStatus;
        result = vk::GetQueryPoolResults(m_pVulkanDecodeContext.dev,
                                         frameSynchronizationInfo.queryPool,
                                         frameSynchronizationInfo.startQueryId,
                                         1,
                                         sizeof(decodeStatus),
                                         &decodeStatus,
                                         sizeof(decodeStatus),
                                         VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

        assert(result == VK_SUCCESS);
        assert(decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

        if (m_dumpDecodeData) {
            std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << currPicIdx << std::endl
                      << "\t\tdecodeStatus: " << decodeStatus << std::endl;
        }
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


    m_decodeFramesData.deinit();

    m_videoSession = nullptr;
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

const char* VkParserVideoPictureParameters::m_refClassId = "VkParserVideoPictureParameters";
int32_t VkParserVideoPictureParameters::m_currentId = 0;

int32_t VkParserVideoPictureParameters::PopulateH264UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                                                 VkVideoDecodeH264SessionParametersAddInfoEXT& h264SessionParametersAddInfo)
{
    int32_t currentId = -1;
    if (pStdPictureParametersSet == NULL) {
        return currentId;
    }

    assert( (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_SPS) ||
            (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_PPS));

    assert(h264SessionParametersAddInfo.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT);

    if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_SPS) {
        h264SessionParametersAddInfo.spsStdCount = 1;
        h264SessionParametersAddInfo.pSpsStd = &pStdPictureParametersSet->m_data.h264Sps.stdSps;
        currentId = pStdPictureParametersSet->m_data.h264Sps.stdSps.seq_parameter_set_id;
    } else if (pStdPictureParametersSet->m_updateType ==  VK_PICTURE_PARAMETERS_UPDATE_H264_PPS ) {
        h264SessionParametersAddInfo.ppsStdCount = 1;
        h264SessionParametersAddInfo.pPpsStd = &pStdPictureParametersSet->m_data.h264Pps.stdPps;
        currentId = pStdPictureParametersSet->m_data.h264Pps.stdPps.pic_parameter_set_id;
    } else {
        assert(!"Incorrect h.264 type");
    }

    return currentId;
}

int32_t VkParserVideoPictureParameters::PopulateH265UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                                                 VkVideoDecodeH265SessionParametersAddInfoEXT& h265SessionParametersAddInfo)
{
    int32_t currentId = -1;
    if (pStdPictureParametersSet == NULL) {
        return currentId;
    }

    assert( (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_SPS) ||
            (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_PPS));

    assert(h265SessionParametersAddInfo.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT);

    if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_SPS) {
        h265SessionParametersAddInfo.spsStdCount = 1;
        h265SessionParametersAddInfo.pSpsStd = &pStdPictureParametersSet->m_data.h265Sps.stdSps;
        currentId = pStdPictureParametersSet->m_data.h265Sps.stdSps.sps_seq_parameter_set_id;
    } else if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_PPS) {
        h265SessionParametersAddInfo.ppsStdCount = 1;
        h265SessionParametersAddInfo.pPpsStd = &pStdPictureParametersSet->m_data.h265Pps.stdPps;
        currentId = pStdPictureParametersSet->m_data.h265Pps.stdPps.pps_seq_parameter_set_id;
    } else {
        assert(!"Incorrect h.265 type");
    }

    return currentId;
}

VkParserVideoPictureParameters*
VkParserVideoPictureParameters::Create(VkDevice device, VkSharedBaseObj<NvVideoSession>& videoSession,
                                       const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                                       const StdVideoPictureParametersSet* pPpsStdPictureParametersSet,
                                       VkParserVideoPictureParameters* pTemplate)
{
    VkParserVideoPictureParameters* pPictureParameters = new VkParserVideoPictureParameters(device);
    if (!pPictureParameters) {
        return pPictureParameters;
    }

    int32_t currentSpsId = -1;
    int32_t currentPpsId = -1;
    const VkParserVideoPictureParameters* pTemplatePictureParameters = pTemplate;
            // VkParserVideoPictureParameters::VideoPictureParametersFromBase(pStdPictureParametersSet->m_vkObjectOwner);

    VkVideoSessionParametersCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR };

    VkVideoDecodeH264SessionParametersCreateInfoEXT h264SessionParametersCreateInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT};
    VkVideoDecodeH264SessionParametersAddInfoEXT h264SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT };

    VkVideoDecodeH265SessionParametersCreateInfoEXT h265SessionParametersCreateInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT };
    VkVideoDecodeH265SessionParametersAddInfoEXT h265SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT};

    VkParserPictureParametersUpdateType updateType = pSpsStdPictureParametersSet ? pSpsStdPictureParametersSet->m_updateType : pPpsStdPictureParametersSet->m_updateType;
    switch (updateType)
    {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        {

            createInfo.pNext =  &h264SessionParametersCreateInfo;

            h264SessionParametersCreateInfo.maxSpsStdCount = MAX_SPS_IDS;
            h264SessionParametersCreateInfo.maxPpsStdCount = MAX_PPS_IDS;
            h264SessionParametersCreateInfo.pParametersAddInfo = &h264SessionParametersAddInfo;

            currentSpsId = PopulateH264UpdateFields(pSpsStdPictureParametersSet, h264SessionParametersAddInfo);
            currentPpsId = PopulateH264UpdateFields(pPpsStdPictureParametersSet, h264SessionParametersAddInfo);

        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
        {
            // Vulkan Video Decode APIs do not support VPS parameters
            return nullptr;
        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
        {

            createInfo.pNext =  &h265SessionParametersCreateInfo;

            h265SessionParametersCreateInfo.maxSpsStdCount = MAX_SPS_IDS;
            h265SessionParametersCreateInfo.maxPpsStdCount = MAX_PPS_IDS;
            h265SessionParametersCreateInfo.pParametersAddInfo = &h265SessionParametersAddInfo;

            currentSpsId = PopulateH265UpdateFields(pSpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentPpsId = PopulateH265UpdateFields(pPpsStdPictureParametersSet, h265SessionParametersAddInfo);

        }
        break;
        default:
            assert(!"Invalid Parser format");
            return NULL;
    }

    createInfo.videoSessionParametersTemplate = pTemplatePictureParameters ? *pTemplatePictureParameters : VkVideoSessionParametersKHR();
    createInfo.videoSession = videoSession->GetVideoSession();;
    VkResult result = vk::CreateVideoSessionParametersKHR(device,
                                                          &createInfo,
                                                          nullptr,
                                                          &pPictureParameters->m_sessionParameters);

    if (result != VK_SUCCESS) {
        assert(!"Could not create Session Parameters Object");
        delete pPictureParameters;
        pPictureParameters = nullptr;
    } else {

        pPictureParameters->m_videoSession = videoSession;

        if (pTemplatePictureParameters) {
            pPictureParameters->m_spsIdsUsed = pTemplatePictureParameters->m_spsIdsUsed;
            pPictureParameters->m_ppsIdsUsed = pTemplatePictureParameters->m_ppsIdsUsed;
        }

        assert ((currentSpsId >= 0) || (currentPpsId >= 0));
        if (currentSpsId >= 0) {
            pPictureParameters->m_spsIdsUsed.set(currentSpsId, true);
        }

        if (currentPpsId >= 0) {
            pPictureParameters->m_ppsIdsUsed.set(currentPpsId, true);
        }

        pPictureParameters->m_Id = ++m_currentId;
    }

    return pPictureParameters;
}

VkResult VkParserVideoPictureParameters::Update(const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                                                const StdVideoPictureParametersSet* pPpsStdPictureParametersSet)
{
    int32_t currentSpsId = -1;
    int32_t currentPpsId = -1;

    VkVideoSessionParametersUpdateInfoKHR updateInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR };
    VkVideoDecodeH264SessionParametersAddInfoEXT h264SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT };
    VkVideoDecodeH265SessionParametersAddInfoEXT h265SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT};

    VkParserPictureParametersUpdateType updateType = pSpsStdPictureParametersSet ? pSpsStdPictureParametersSet->m_updateType : pPpsStdPictureParametersSet->m_updateType;
    switch (updateType)
    {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        {

            updateInfo.pNext = &h264SessionParametersAddInfo;

            currentSpsId = PopulateH264UpdateFields(pSpsStdPictureParametersSet, h264SessionParametersAddInfo);
            currentPpsId = PopulateH264UpdateFields(pPpsStdPictureParametersSet, h264SessionParametersAddInfo);

        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
        {
            // Vulkan Video Decode APIs do not support VPS parameters
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
        {

            updateInfo.pNext = &h265SessionParametersAddInfo;

            currentSpsId = PopulateH265UpdateFields(pSpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentPpsId = PopulateH265UpdateFields(pPpsStdPictureParametersSet, h265SessionParametersAddInfo);

        }
        break;
        default:
            assert(!"Invalid Parser format");
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pSpsStdPictureParametersSet) {
        updateInfo.updateSequenceCount = std::max(pSpsStdPictureParametersSet->m_updateSequenceCount, updateInfo.updateSequenceCount);
    }

    if (pPpsStdPictureParametersSet) {
        updateInfo.updateSequenceCount = std::max(pPpsStdPictureParametersSet->m_updateSequenceCount, updateInfo.updateSequenceCount);
    }

    VkResult result = vk::UpdateVideoSessionParametersKHR(m_device,
                                                          m_sessionParameters,
                                                          &updateInfo);

    if (result == VK_SUCCESS) {

        assert ((currentSpsId >= 0) || (currentPpsId >= 0));
        if (currentSpsId >= 0) {
            m_spsIdsUsed.set(currentSpsId, true);
        }
        if (currentPpsId >= 0) {
            m_ppsIdsUsed.set(currentPpsId, true);
        }

    } else {
        assert(!"Could not update Session Parameters Object");
    }

    return result;
}

VkParserVideoPictureParameters::~VkParserVideoPictureParameters()
{
    if (m_sessionParameters) {
        vk::DestroyVideoSessionParametersKHR(m_device, m_sessionParameters, nullptr);
        m_sessionParameters = NULL;
    }
    m_videoSession = nullptr;
}

int32_t VkParserVideoPictureParameters::AddRef()
{
    return ++m_refCount;
}

int32_t VkParserVideoPictureParameters::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}

const char* StdVideoPictureParametersSet::m_refClassId = "StdVideoPictureParametersSet";
