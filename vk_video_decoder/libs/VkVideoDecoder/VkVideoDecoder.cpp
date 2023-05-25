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

#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "VkVideoDecoder/VkVideoDecoder.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

#undef max
#undef min

#define GPU_ALIGN(x) (((x) + 0xff) & ~0xff)

const uint64_t gFenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;
const uint64_t gLongTimeout  = 1000 * 1000 * 1000 /* 1000 mSec */;

const char* VkVideoDecoder::GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec)
{
    static struct {
        VkVideoCodecOperationFlagBitsKHR eCodec;
        const char* name;
    } aCodecName[] = {
        { VK_VIDEO_CODEC_OPERATION_NONE_KHR, "None" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "AVC/H.264" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "H.265/HEVC" },
#ifdef VK_EXT_video_decode_vp9
        { VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9" },
#endif // VK_EXT_video_decode_vp9
#ifdef vulkan_video_codec_av1std
        { VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1" },
#endif // VK_EXT_video_decode_av1
    };

    for (unsigned i = 0; i < sizeof(aCodecName) / sizeof(aCodecName[0]); i++) {
        if (codec == aCodecName[i].eCodec) {
            return aCodecName[i].name;
        }
    }

    return "Unknown";
}

const char* VkVideoDecoder::GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat)
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

/* Callback function to be registered for getting a callback when decoding of
 * sequence starts. Return value from HandleVideoSequence() are interpreted as :
 *  0: fail, 1: suceeded, > 1: override dpb size of parser (set by
 * nvVideoParseParameters::ulMaxNumDecodeSurfaces while creating parser)
 */
int32_t VkVideoDecoder::StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat)
{
    const bool testUseLargestSurfaceExtent = false;
    // Assume 4k content for testing surfaces
    const uint32_t surfaceMinWidthExtent  = 4096;
    const uint32_t surfaceMinHeightExtent = 4096;

    VkExtent2D codedExtent = { pVideoFormat->coded_width, pVideoFormat->coded_height };

    // Width and height of the image surface
    VkExtent2D imageExtent = VkExtent2D { std::max((uint32_t)(pVideoFormat->display_area.right  - pVideoFormat->display_area.left), pVideoFormat->coded_width),
                                          std::max((uint32_t)(pVideoFormat->display_area.bottom - pVideoFormat->display_area.top),  pVideoFormat->coded_height) };

    // If we are testing content with different sizes against max sized surface vs. images dynamic resize
    // then set the imageExtent to the max surface size selected.
    if (testUseLargestSurfaceExtent) {
        imageExtent = { std::max(surfaceMinWidthExtent,  imageExtent.width),
                        std::max(surfaceMinHeightExtent, imageExtent.height) };
    }

    std::cout << "Video Input Information" << std::endl
              << "\tCodec        : " << GetVideoCodecString(pVideoFormat->codec) << std::endl
              << "\tFrame rate   : " << pVideoFormat->frame_rate.numerator << "/" << pVideoFormat->frame_rate.denominator << " = "
              << ((pVideoFormat->frame_rate.denominator != 0) ? (1.0 * pVideoFormat->frame_rate.numerator / pVideoFormat->frame_rate.denominator) : 0.0) << " fps" << std::endl
              << "\tSequence     : " << (pVideoFormat->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
              << "\tCoded size   : [" << codedExtent.width << ", " << codedExtent.height << "]" << std::endl
              << "\tDisplay area : [" << pVideoFormat->display_area.left << ", " << pVideoFormat->display_area.top << ", "
              << pVideoFormat->display_area.right << ", " << pVideoFormat->display_area.bottom << "]" << std::endl
              << "\tChroma       : " << GetVideoChromaFormatString(pVideoFormat->chromaSubsampling) << std::endl
              << "\tBit depth    : " << pVideoFormat->bit_depth_luma_minus8 + 8 << std::endl;

    m_numDecodeSurfaces = std::max(m_numDecodeSurfaces, (pVideoFormat->minNumDecodeSurfaces + m_numDecodeImagesInFlight));

    VkResult result = VK_SUCCESS;

    int32_t videoQueueFamily = m_vkDevCtx->GetVideoDecodeQueueFamilyIdx();
    VkVideoCodecOperationFlagsKHR videoCodecs = VulkanVideoCapabilities::GetSupportedCodecs(m_vkDevCtx,
            m_vkDevCtx->getPhysicalDevice(),
            &videoQueueFamily,
            VK_QUEUE_VIDEO_DECODE_BIT_KHR,
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR
            | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR
#ifdef ENABLE_AV1_DECODER
            | VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR
#endif
    );
    assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_NONE_KHR);

    if (m_dumpDecodeData) {
        std::cout << "\t" << std::hex << videoCodecs << " HW codec types are available: " << std::dec << std::endl;
    }

    VkVideoCodecOperationFlagBitsKHR videoCodec = pVideoFormat->codec;

    if (m_dumpDecodeData) {
        std::cout << "\tcodec " << VkVideoCoreProfile::CodecToName(videoCodec) << std::endl;
    }

    VkVideoCoreProfile videoProfile(videoCodec, pVideoFormat->chromaSubsampling, pVideoFormat->lumaBitDepth, pVideoFormat->chromaBitDepth,
                                    pVideoFormat->codecProfile);
#ifndef HEADLESS_AV1
    if (!VulkanVideoCapabilities::IsCodecTypeSupported(m_vkDevCtx,
                                                       m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                                       videoCodec)) {
        std::cout << "*** The video codec " << VkVideoCoreProfile::CodecToName(videoCodec) << " is not supported! ***" << std::endl;
        assert(!"The video codec is not supported");
        return -1;
    }
#endif

    if (m_videoFormat.coded_width && m_videoFormat.coded_height) {
        // CreateDecoder() has been called before, and now there's possible config change
        m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_currentVideoQueueIndx);

        if (*m_vkDevCtx) {
            m_vkDevCtx->DeviceWaitIdle();
        }
    }

    std::cout << "Video Decoding Params:" << std::endl
              << "\tNum Surfaces : " << m_numDecodeSurfaces << std::endl
              << "\tResize       : " << codedExtent.width << " x " << codedExtent.height << std::endl;

    uint32_t maxDpbSlotCount = pVideoFormat->maxNumDpbSlots;

    assert(VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR == pVideoFormat->chromaSubsampling);


    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoDecodeCapabilitiesKHR videoDecodeCapabilities;
    result = VulkanVideoCapabilities::GetVideoDecodeCapabilities(m_vkDevCtx, videoProfile,
                                                                 videoCapabilities,
                                                                 videoDecodeCapabilities);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return -1;
    }
    m_capabilityFlags = videoDecodeCapabilities.flags;
    m_dpbAndOutputCoincide = (m_capabilityFlags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
    VkFormat dpbImageFormat = VK_FORMAT_UNDEFINED;
    VkFormat outImageFormat = VK_FORMAT_UNDEFINED;
    result = VulkanVideoCapabilities::GetSupportedVideoFormats(m_vkDevCtx, videoProfile,
                                                               m_capabilityFlags,
                                                               outImageFormat,
                                                               dpbImageFormat);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get supported video formats :" << result << " ***" << std::endl;
        assert(!"Could not get supported video formats!");
        return -1;
    }

    imageExtent.width  = std::max(imageExtent.width, videoCapabilities.minCodedExtent.width);
    imageExtent.height = std::max(imageExtent.height, videoCapabilities.minCodedExtent.height);

    uint32_t alignWidth = videoCapabilities.pictureAccessGranularity.width - 1;
    imageExtent.width = ((imageExtent.width + alignWidth) & ~alignWidth);
    uint32_t alignHeight = videoCapabilities.pictureAccessGranularity.height - 1;
    imageExtent.height = ((imageExtent.height + alignHeight) & ~alignHeight);

    // TODO: questions about slot mgmt.
    // maxDpbSlotCount = std::min(videoCapabilities.maxDpbSlots, maxDpbSlotCount);)

    if (!m_videoSession ||
            !m_videoSession->IsCompatible( m_vkDevCtx,
                                           m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                           &videoProfile,
                                           outImageFormat,
                                           imageExtent,
                                           dpbImageFormat,
                                           maxDpbSlotCount,
                                           std::max<uint32_t>(maxDpbSlotCount, VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS)) ) {

        result = VulkanVideoSession::Create( m_vkDevCtx,
                                             m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                             &videoProfile,
                                             outImageFormat,
                                             imageExtent,
                                             dpbImageFormat,
                                             maxDpbSlotCount,
                                             std::min<uint32_t>(maxDpbSlotCount, VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS),
                                             m_videoSession);

        // after creating a new video session, we need codec reset.
        m_resetDecoder = true;
        assert(result == VK_SUCCESS);
    }

    VkImageUsageFlags outImageUsage = (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                                       VK_IMAGE_USAGE_SAMPLED_BIT      |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageUsageFlags dpbImageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;

    if (m_dpbAndOutputCoincide) {
        dpbImageUsage = outImageUsage | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
        outImageUsage &= ~VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
    } else {
        // The implementation does not support dpbAndOutputCoincide
        m_useSeparateOutputImages = true;
    }

    if(!(videoCapabilities.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR)) {
        // The implementation does not support individual images for DPB and so must use arrays
        m_useImageArray = true;
        m_useImageViewArray = true;
    }

    int32_t ret = m_videoFrameBuffer->InitImagePool(videoProfile.GetProfile(),
                                                    m_numDecodeSurfaces,
                                                    dpbImageFormat,
                                                    outImageFormat,
                                                    codedExtent,
                                                    imageExtent,
                                                    dpbImageUsage,
                                                    outImageUsage,
                                                    m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                                    m_numDecodeImagesToPreallocate,
                                                    m_useImageArray, m_useImageViewArray,
                                                    m_useSeparateOutputImages, m_useLinearOutput);

    assert((uint32_t)ret == m_numDecodeSurfaces);
    if ((uint32_t)ret != m_numDecodeSurfaces) {
        fprintf(stderr, "\nERROR: InitImagePool() ret(%d) != m_numDecodeSurfaces(%d)\n", ret, m_numDecodeSurfaces);
    }

    if (m_dumpDecodeData) {
        std::cout << "Allocating Video Device Memory" << std::endl
                  << "Allocating " << m_numDecodeSurfaces << " Num Decode Surfaces and "
                  << maxDpbSlotCount << " Video Device Memory Images for DPB " << std::endl
                  << imageExtent.width << " x " << imageExtent.height << std::endl;
    }
    m_maxDecodeFramesCount = m_numDecodeSurfaces;

    // There will be no more than 32 frames in the queue.
    m_decodeFramesData.resize(std::max<uint32_t>(m_maxDecodeFramesCount, 32));


    int32_t availableBuffers = (int32_t)m_decodeFramesData.GetBitstreamBuffersQueue().
                                                      GetAvailableNodesNumber();
    if (availableBuffers < m_numBitstreamBuffersToPreallocate) {

        uint32_t allocateNumBuffers = std::min<uint32_t>(
                m_decodeFramesData.GetBitstreamBuffersQueue().GetMaxNodes(),
                (m_numBitstreamBuffersToPreallocate - availableBuffers));

        allocateNumBuffers = std::min<uint32_t>(allocateNumBuffers,
                m_decodeFramesData.GetBitstreamBuffersQueue().GetFreeNodesNumber());

        for (uint32_t i = 0; i < allocateNumBuffers; i++) {

            VkSharedBaseObj<VulkanBitstreamBufferImpl> bitstreamBuffer;
            VkDeviceSize allocSize = std::max<VkDeviceSize>(m_maxStreamBufferSize, 2 * 1024 * 1024);

            result = VulkanBitstreamBufferImpl::Create(m_vkDevCtx,
                    m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                    allocSize,
                    videoCapabilities.minBitstreamBufferOffsetAlignment,
                    videoCapabilities.minBitstreamBufferSizeAlignment,
                    nullptr, 0, bitstreamBuffer);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
                break;
            }

            int32_t nodeAddedWithIndex = m_decodeFramesData.GetBitstreamBuffersQueue().
                                                 AddNodeToPool(bitstreamBuffer, false);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
                break;
            }
        }
    }

    // Save the original config
    m_videoFormat = *pVideoFormat;
    return m_numDecodeSurfaces;
}

bool VkVideoDecoder::UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersObject,
                                             VkSharedBaseObj<VkVideoRefCountBase>& client)
{
    VkResult result = VkParserVideoPictureParameters::AddPictureParameters(m_vkDevCtx,
                                                                           m_videoSession,
                                                                           pictureParametersObject,
                                                                           m_currentPictureParameters);

    client = m_currentPictureParameters;
    return (result == VK_SUCCESS);
}


int VkVideoDecoder::CopyOptimalToLinearImage(VkCommandBuffer& commandBuffer,
                                          VkVideoPictureResourceInfoKHR& srcPictureResource,
                                          VulkanVideoFrameBuffer::PictureResourceInfo& srcPictureResourceInfo,
                                          VkVideoPictureResourceInfoKHR& dstPictureResource,
                                          VulkanVideoFrameBuffer::PictureResourceInfo& dstPictureResourceInfo,
                                          VulkanVideoFrameBuffer::FrameSynchronizationInfo *pFrameSynchronizationInfo)

{
    // Bind memory for the image.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(srcPictureResourceInfo.imageFormat);

    // Currently formats that have more than 2 output planes are not supported. 444 formats have a shared CbCr planes in all current tests
    assert((mpInfo->vkPlaneFormat[2] == VK_FORMAT_UNDEFINED) && (mpInfo->vkPlaneFormat[3] == VK_FORMAT_UNDEFINED));

    // Copy src buffer to image.
    VkImageCopy copyRegion[3];
    memset(&copyRegion, 0, sizeof(copyRegion));
    copyRegion[0].extent.width = srcPictureResource.codedExtent.width;
    copyRegion[0].extent.height = srcPictureResource.codedExtent.height;
    copyRegion[0].extent.depth = 1;
    copyRegion[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].srcSubresource.mipLevel = 0;
    copyRegion[0].srcSubresource.baseArrayLayer = srcPictureResource.baseArrayLayer;
    copyRegion[0].srcSubresource.layerCount = 1;
    copyRegion[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].dstSubresource.mipLevel = 0;
    copyRegion[0].dstSubresource.baseArrayLayer = dstPictureResource.baseArrayLayer;
    copyRegion[0].dstSubresource.layerCount = 1;
    copyRegion[1].extent.width = copyRegion[0].extent.width;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledX != 0) {
        copyRegion[1].extent.width /= 2;
    }

    copyRegion[1].extent.height = copyRegion[0].extent.height;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledY != 0) {
        copyRegion[1].extent.height /= 2;
    }

    copyRegion[1].extent.depth = 1;
    copyRegion[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].srcSubresource.mipLevel = 0;
    copyRegion[1].srcSubresource.baseArrayLayer = srcPictureResource.baseArrayLayer;
    copyRegion[1].srcSubresource.layerCount = 1;
    copyRegion[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].dstSubresource.mipLevel = 0;
    copyRegion[1].dstSubresource.baseArrayLayer = dstPictureResource.baseArrayLayer;
    copyRegion[1].dstSubresource.layerCount = 1;

    m_vkDevCtx->CmdCopyImage(commandBuffer, srcPictureResourceInfo.image, srcPictureResourceInfo.currentImageLayout,
                                    dstPictureResourceInfo.image, dstPictureResourceInfo.currentImageLayout,
                                    (uint32_t)2, copyRegion);

    {
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               1, &memoryBarrier, 0,
                                0, 0, 0);
    }

    return 0;
}

/* Callback function to be registered for getting a callback when a decoded
 * frame is ready to be decoded. Return value from HandlePictureDecode() are
 * interpreted as: 0: fail, >=1: suceeded
 */
int VkVideoDecoder::DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pCurrFrameDecParams,
                                                VkParserDecodePictureInfo* pDecodePictureInfo)
{
    if (!m_videoSession) {
        assert(!"Decoder not initialized!");
        return -1;
    }

    int32_t currPicIdx = pCurrFrameDecParams->currPicIdx;
    assert((uint32_t)currPicIdx < m_numDecodeSurfaces);

    int32_t picNumInDecodeOrder = (int32_t)(uint32_t)m_decodePicCount;
    if (m_dumpDecodeData) {
        std::cout << "currPicIdx: " << currPicIdx << ", currentVideoQueueIndx: " << m_currentVideoQueueIndx << ", decodePicCount: " << m_decodePicCount << std::endl;
    }
    m_videoFrameBuffer->SetPicNumInDecodeOrder(currPicIdx, picNumInDecodeOrder);

    NvVkDecodeFrameDataSlot frameDataSlot;
    int32_t retPicIdx = GetCurrentFrameData((uint32_t)currPicIdx, frameDataSlot);
    assert(retPicIdx == currPicIdx);

    if (retPicIdx != currPicIdx) {
        fprintf(stderr, "\nERROR: DecodePictureWithParameters() retPicIdx(%d) != currPicIdx(%d)\n", retPicIdx, currPicIdx);
    }

    assert(pCurrFrameDecParams->bitstreamData->GetMaxSize() >= pCurrFrameDecParams->bitstreamDataLen);

    // VkDeviceSize maxSize = 0;
    // const uint8_t* ptr = pCurrFrameDecParams->bitstreamData->GetReadOnlyDataPtr(0, maxSize);
    // printf("Charlie:: NVIDIA BEFORE: bitstreamDataLen= %d max=%d\n", 	pCurrFrameDecParams->bitstreamDataLen, maxSize);
    // for (int i = 0 ; i< pCurrFrameDecParams->bitstreamDataLen; i++)
    // 	printf("0x%02x, ", ptr[i]);
    // printf("\n");

    // uint8_t* bitstreamDataPtr = pCurrFrameDecParams->bitstreamData->GetDataPtr(0, maxSize);
    // uint8_t bitstream[] = { 0x9e, 0x86, 0x5b, 0xb2, 0x22, 0xb5, 0x58, 0x4d, 0x68, 0xe6, 0x37, 0x54, 0x42, 0x7b, 0x84, 0xce, 0xdf, 0x9f, 0xec, 0xab, 0x07, 0x4d, 0xf6, 0xe1, 0x5e, 0x9e, 0x27, 0xbf, 0x93, 0x2f, 0x47, 0x0d, 0x7b, 0x7c, 0x45, 0x8d, 0xcf, 0x26, 0xf7, 0x6c, 0x06, 0xd7, 0x8c, 0x2e, 0xf5, 0x2c, 0xb0, 0x8a, 0x31, 0xac, 0x69, 0xf5, 0xcd, 0xd8, 0x71, 0x5d, 0xaf, 0xf8, 0x96, 0x43, 0x8c, 0x9c, 0x23, 0x6f, 0xab, 0xd0, 0x35, 0x43, 0xdf, 0x81, 0x12, 0xe3, 0x7d, 0xec, 0x22, 0xb0, 0x30, 0x54, 0x32, 0x9f, 0x90, 0xc0, 0x5d, 0x64, 0x9b, 0x0f, 0x75, 0x31, 0x84, 0x3a, 0x57, 0xd7, 0x5f, 0x03, 0x6e, 0x7f, 0x43, 0x17, 0x6d, 0x08, 0xc3, 0x81, 0x8a, 0xae, 0x73, 0x1c, 0xa8, 0xa7, 0xe4, 0x9c, 0xa9, 0x5b, 0x3f, 0xd1, 0xeb, 0x75, 0x3a, 0x7f, 0x22, 0x77, 0x38, 0x64, 0x1c, 0x77, 0xdb, 0xcd, 0xef, 0xb7, 0x08, 0x45, 0x8e, 0x7f, 0xea, 0xa3, 0xd0, 0x81, 0xc9, 0xc1, 0xbc, 0x93, 0x9b, 0x41, 0xb1, 0xa1, 0x42, 0x17, 0x98, 0x3f, 0x1e, 0x95, 0xdf, 0x68, 0x7c, 0xb7, 0x98 };
    // //memcpy(bitstreamDataPtr, bitstream, sizeof(bitstream));
    // //pCurrFrameDecParams->bitstreamData->FlushRange(0, VK_WHOLE_SIZE);

    // ptr = pCurrFrameDecParams->bitstreamData->GetReadOnlyDataPtr(0, maxSize);
    // printf("Charlie::::: NVIDIA AFTER: bitstreamDataLen= %d max=%d\n", 	pCurrFrameDecParams->bitstreamDataLen, maxSize);
    // for (int i = 0 ; i< pCurrFrameDecParams->bitstreamDataLen;i++)
    // 	printf("%02x ", ptr[i]);
    // printf("\n");
    pCurrFrameDecParams->decodeFrameInfo.srcBuffer = pCurrFrameDecParams->bitstreamData->GetBuffer();
    assert(pCurrFrameDecParams->bitstreamDataOffset == 0);
    assert(pCurrFrameDecParams->firstSliceIndex == 0);
    pCurrFrameDecParams->decodeFrameInfo.srcBufferOffset = pCurrFrameDecParams->bitstreamDataOffset;
    pCurrFrameDecParams->decodeFrameInfo.srcBufferRange =  (pCurrFrameDecParams->bitstreamDataLen + 127) & ~127; // TODO: alignment macro / wiring the alignment requirement in.
    // pCurrFrameDecParams->decodeFrameInfo.dstImageView = VkImageView();

    VkVideoBeginCodingInfoKHR decodeBeginInfo = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    // CmdResetQueryPool are NOT Supported yet.
    decodeBeginInfo.pNext = pCurrFrameDecParams->beginCodingInfoPictureParametersExt;

    decodeBeginInfo.videoSession = m_videoSession->GetVideoSession();

    assert(pCurrFrameDecParams->decodeFrameInfo.srcBuffer);
    const VkBufferMemoryBarrier2KHR bitstreamBufferMemoryBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR,
        nullptr,
        VK_PIPELINE_STAGE_2_NONE_KHR,
        VK_ACCESS_2_HOST_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
        VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR,
        VK_QUEUE_FAMILY_IGNORED,
        (uint32_t)m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
        pCurrFrameDecParams->decodeFrameInfo.srcBuffer,
        pCurrFrameDecParams->decodeFrameInfo.srcBufferOffset,
        pCurrFrameDecParams->decodeFrameInfo.srcBufferRange
    };

    uint32_t baseArrayLayer = (m_useImageArray || m_useImageViewArray) ? pCurrFrameDecParams->currPicIdx : 0;
    const VkImageMemoryBarrier2KHR dpbBarrierTemplates[1] = {
        { // VkImageMemoryBarrier

            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr, // const void*     pNext
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR srcStageMask
            0, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask;
            VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR, // VkAccessFlags   dstAccessMask
            VK_IMAGE_LAYOUT_UNDEFINED, // VkImageLayout   oldLayout
            VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, // VkImageLayout   newLayout
            VK_QUEUE_FAMILY_IGNORED, // uint32_t        srcQueueFamilyIndex
            (uint32_t)m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(), // uint32_t   dstQueueFamilyIndex
            VkImage(), // VkImage         image;
            {
                // VkImageSubresourceRange   subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0, // uint32_t           baseMipLevel
                1, // uint32_t           levelCount
                baseArrayLayer, // uint32_t           baseArrayLayer
                1, // uint32_t           layerCount;
            } },
    };

    VkImageMemoryBarrier2KHR imageBarriers[VkParserPerFrameDecodeParameters::MAX_DPB_REF_AND_SETUP_SLOTS];
    uint32_t numDpbBarriers = 0;
    VulkanVideoFrameBuffer::PictureResourceInfo currentDpbPictureResourceInfo = VulkanVideoFrameBuffer::PictureResourceInfo();
    VulkanVideoFrameBuffer::PictureResourceInfo currentOutputPictureResourceInfo = VulkanVideoFrameBuffer::PictureResourceInfo();
    VkVideoPictureResourceInfoKHR currentOutputPictureResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR, nullptr};

    VkVideoPictureResourceInfoKHR* pOutputPictureResource = nullptr;
    VulkanVideoFrameBuffer::PictureResourceInfo* pOutputPictureResourceInfo = nullptr;
    if (!m_dpbAndOutputCoincide) {

        // Output Distinct will use the decodeFrameInfo.dstPictureResource directly.
        pOutputPictureResource = &pCurrFrameDecParams->decodeFrameInfo.dstPictureResource;
    } else if (m_useLinearOutput) {

        // Output Coincide needs the output only if we are processing linear images that we need to copy to below.
        pOutputPictureResource = &currentOutputPictureResource;
    }

    if (pOutputPictureResource) {

        // if the pOutputPictureResource is set then we also need the pOutputPictureResourceInfo.
        pOutputPictureResourceInfo = &currentOutputPictureResourceInfo;

    }

    if (pCurrFrameDecParams->currPicIdx !=
            m_videoFrameBuffer->GetCurrentImageResourceByIndex(pCurrFrameDecParams->currPicIdx,
                                                               &pCurrFrameDecParams->dpbSetupPictureResource,
                                                               &currentDpbPictureResourceInfo,
                                                               VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                                                               pOutputPictureResource,
                                                               pOutputPictureResourceInfo,
                                                               VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR)) {

        assert(!"GetImageResourcesByIndex has failed");
    }

    if (m_dpbAndOutputCoincide) {

        // For the Output Coincide, the DPB and destination output resources are the same.
        pCurrFrameDecParams->decodeFrameInfo.dstPictureResource = pCurrFrameDecParams->dpbSetupPictureResource;

    } else if (pOutputPictureResourceInfo) {

        // For Output Distinct transition the image to DECODE_DST
        if (pOutputPictureResourceInfo->currentImageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
            imageBarriers[numDpbBarriers].oldLayout = pOutputPictureResourceInfo->currentImageLayout;
            imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
            imageBarriers[numDpbBarriers].image = pOutputPictureResourceInfo->image;
            imageBarriers[numDpbBarriers].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
            assert(imageBarriers[numDpbBarriers].image);
            numDpbBarriers++;
        }
    }

    if (currentDpbPictureResourceInfo.currentImageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
        imageBarriers[numDpbBarriers].oldLayout = currentDpbPictureResourceInfo.currentImageLayout;
        imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        imageBarriers[numDpbBarriers].image = currentDpbPictureResourceInfo.image;
        imageBarriers[numDpbBarriers].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        assert(imageBarriers[numDpbBarriers].image);
        numDpbBarriers++;
    }

    VulkanVideoFrameBuffer::PictureResourceInfo pictureResourcesInfo[VkParserPerFrameDecodeParameters::MAX_DPB_REF_AND_SETUP_SLOTS];
    memset(&pictureResourcesInfo[0], 0, sizeof(pictureResourcesInfo));
    const int8_t* pGopReferenceImagesIndexes = pCurrFrameDecParams->pGopReferenceImagesIndexes;
    if (pCurrFrameDecParams->numGopReferenceSlots) {
        if (pCurrFrameDecParams->numGopReferenceSlots != m_videoFrameBuffer->GetDpbImageResourcesByIndex(
                                                                        pCurrFrameDecParams->numGopReferenceSlots,
                                                                        pGopReferenceImagesIndexes,
                                                                        pCurrFrameDecParams->pictureResources,
                                                                        pictureResourcesInfo,
                                                                        VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR)) {
            assert(!"GetImageResourcesByIndex has failed");
        }
        for (int32_t resId = 0; resId < pCurrFrameDecParams->numGopReferenceSlots; resId++) {
            // slotLayer requires NVIDIA specific extension VK_KHR_video_layers, not enabled, just yet.
            // pGopReferenceSlots[resId].slotLayerIndex = 0;
            // pictureResourcesInfo[resId].image can be a nullptr handle if the picture is not-existent.
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

    decodeBeginInfo.referenceSlotCount = pCurrFrameDecParams->decodeFrameInfo.referenceSlotCount;
    if (pCurrFrameDecParams->isAV1) {
        // AV1 always keeps a setup slot active.
        //pCurrFrameDecParams->decodeFrameInfo.referenceSlotCount--;
    }    
    decodeBeginInfo.pReferenceSlots = pCurrFrameDecParams->decodeFrameInfo.pReferenceSlots;
    
    if (false) {
        for (unsigned i = 0; i < pCurrFrameDecParams->decodeFrameInfo.referenceSlotCount; i++) {
            printf("pCurrFrameDecParams->decodeFrameInfo.pReferenceSlots[%d].slotIndex = %d (%p)\n", i, pCurrFrameDecParams->decodeFrameInfo.pReferenceSlots[i].slotIndex, pCurrFrameDecParams->decodeFrameInfo.pReferenceSlots[i].pPictureResource->imageViewBinding);
        }
    }
    
    if (pDecodePictureInfo->flags.unpairedField) {
        // assert(pFrameSyncinfo->frameCompleteSemaphore == VkSemaphore());
        pDecodePictureInfo->flags.syncFirstReady = true;
    }
    // FIXME: the below sequence for interlaced synchronization.
    pDecodePictureInfo->flags.syncToFirstField = false;

    VulkanVideoFrameBuffer::FrameSynchronizationInfo frameSynchronizationInfo = VulkanVideoFrameBuffer::FrameSynchronizationInfo();
    frameSynchronizationInfo.hasFrameCompleteSignalFence = true;
    frameSynchronizationInfo.hasFrameCompleteSignalSemaphore = true;

    if (pCurrFrameDecParams->useInlinedPictureParameters == false) {
        if (m_videoFormat.codec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR || m_videoFormat.codec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
            // out of band parameters
            VkSharedBaseObj<VkVideoRefCountBase> currentVkPictureParameters;
            bool valid = pCurrFrameDecParams->pStdPps->GetClientObject(currentVkPictureParameters);
            assert(currentVkPictureParameters && valid);
            if (!(currentVkPictureParameters && valid)) {
                return -1;
            }
            VkParserVideoPictureParameters* pOwnerPictureParameters =
                VkParserVideoPictureParameters::VideoPictureParametersFromBase(currentVkPictureParameters);
            assert(pOwnerPictureParameters);
            assert(pOwnerPictureParameters->GetId() <= m_currentPictureParameters->GetId());
            int32_t ret = pOwnerPictureParameters->FlushPictureParametersQueue(m_videoSession);
            assert(ret >= 0);
            if (!(ret >= 0)) {
                return -1;
            }
            bool isSps = false;
            int32_t spsId = pCurrFrameDecParams->pStdPps->GetSpsId(isSps);
            assert(!isSps);
            assert(spsId >= 0);
            assert(pOwnerPictureParameters->HasSpsId(spsId));
            bool isPps = false;
            int32_t ppsId = pCurrFrameDecParams->pStdPps->GetPpsId(isPps);
            assert(isPps);
            assert(ppsId >= 0);
            assert(pOwnerPictureParameters->HasPpsId(ppsId));

            decodeBeginInfo.videoSessionParameters = *pOwnerPictureParameters;

            if (m_dumpDecodeData) {
                std::cout << "Using object " << decodeBeginInfo.videoSessionParameters <<
                    " with ID: (" << pOwnerPictureParameters->GetId() << ")" <<
                    " for SPS: " <<  spsId << ", PPS: " << ppsId << std::endl;
            }
        } else if (m_videoFormat.codec == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) { // AV1
            VkSharedBaseObj<VkVideoRefCountBase> currentVkPictureParameters;
            bool valid = pCurrFrameDecParams->pStdAv1Sps->GetClientObject(currentVkPictureParameters);
            assert(valid);
            VkParserVideoPictureParameters* pOwnerPictureParameters =
                VkParserVideoPictureParameters::VideoPictureParametersFromBase(currentVkPictureParameters);


            bool isAv1Sps = false;
            int32_t av1SpsId = pCurrFrameDecParams->pStdAv1Sps->GetAv1SpsId(isAv1Sps);
            assert(isAv1Sps);
            assert(pOwnerPictureParameters);
            assert(pOwnerPictureParameters->GetId() <= m_currentPictureParameters->GetId());
            int32_t ret = pOwnerPictureParameters->FlushPictureParametersQueue(m_videoSession);
            assert(ret >= 0);
            if (!(ret >= 0)) {
                return -1;
            }
            assert(pOwnerPictureParameters->HasAv1PpsId(av1SpsId));

            decodeBeginInfo.videoSessionParameters = *pOwnerPictureParameters;

        }
    } else {
        decodeBeginInfo.videoSessionParameters = VK_NULL_HANDLE;
    }

    VulkanVideoFrameBuffer::ReferencedObjectsInfo referencedObjectsInfo(pCurrFrameDecParams->bitstreamData,
                                                                        pCurrFrameDecParams->pStdPps,
                                                                        pCurrFrameDecParams->pStdSps,
                                                                        pCurrFrameDecParams->pStdVps,
                                                                        pCurrFrameDecParams->pStdAv1Sps);
    int32_t retVal = m_videoFrameBuffer->QueuePictureForDecode(currPicIdx, pDecodePictureInfo,
                                                               &referencedObjectsInfo,
                                                               &frameSynchronizationInfo);
    if (currPicIdx != retVal) {
        assert(!"QueuePictureForDecode has failed");
    }

    VkFence frameCompleteFence = frameSynchronizationInfo.frameCompleteFence;
    VkFence frameConsumerDoneFence = frameSynchronizationInfo.frameConsumerDoneFence;
    VkSemaphore frameCompleteSemaphore = frameSynchronizationInfo.frameCompleteSemaphore;
    VkSemaphore frameConsumerDoneSemaphore = frameSynchronizationInfo.frameConsumerDoneSemaphore;

    // Check here that the frame for this entry (for this command buffer) has already completed decoding.
    // Otherwise we may step over a hot command buffer by starting a new recording.
    // This fence wait should be NOP in 99.9% of the cases, because the decode queue is deep enough to
    // ensure the frame has already been completed.
    assert(frameCompleteFence != VK_NULL_HANDLE);
    VkResult result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
    if (result != VK_SUCCESS) {
        std::cerr << "\t *************** WARNING: frameCompleteFence is not done *************< " << currPicIdx << " >**********************" << std::endl;
        assert(!"frameCompleteFence is not signaled yet after more than 100 mSec wait");
    }

    result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
    if (result == VK_NOT_READY) {
        std::cerr << "\t *************** WARNING: frameCompleteFence is not done *************< " << currPicIdx << " >**********************" << std::endl;
        assert(!"frameCompleteFence is not signaled yet");
    }


    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;

    m_vkDevCtx->BeginCommandBuffer(frameDataSlot.commandBuffer, &beginInfo);

    // m_vkDevCtx->ResetQueryPool(m_vkDev, queryFrameInfo.queryPool, queryFrameInfo.query, 1);

    if (frameSynchronizationInfo.queryPool) {
        m_vkDevCtx->CmdResetQueryPool(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool,
                                      frameSynchronizationInfo.startQueryId, frameSynchronizationInfo.numQueries);
    }

    m_vkDevCtx->CmdBeginVideoCodingKHR(frameDataSlot.commandBuffer, &decodeBeginInfo);

    if (m_resetDecoder != false) {
        VkVideoCodingControlInfoKHR codingControlInfo = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
                                                          nullptr,
                                                          VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR };

        // Video spec requires mandatory codec reset before the first frame.
        m_vkDevCtx->CmdControlVideoCodingKHR(frameDataSlot.commandBuffer, &codingControlInfo);
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
    m_vkDevCtx->CmdPipelineBarrier2KHR(frameDataSlot.commandBuffer, &dependencyInfo);

    if (frameSynchronizationInfo.queryPool != VK_NULL_HANDLE) {
        m_vkDevCtx->CmdBeginQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool,
                                  frameSynchronizationInfo.startQueryId, VkQueryControlFlags());
    }

    m_vkDevCtx->CmdDecodeVideoKHR(frameDataSlot.commandBuffer, &pCurrFrameDecParams->decodeFrameInfo);

    if (frameSynchronizationInfo.queryPool != VK_NULL_HANDLE) {
        m_vkDevCtx->CmdEndQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool,
                                frameSynchronizationInfo.startQueryId);
    }

    VkVideoEndCodingInfoKHR decodeEndInfo = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    m_vkDevCtx->CmdEndVideoCodingKHR(frameDataSlot.commandBuffer, &decodeEndInfo);

    if (m_dpbAndOutputCoincide && (m_useSeparateOutputImages || m_useLinearOutput)) {
        CopyOptimalToLinearImage(frameDataSlot.commandBuffer,
                                 pCurrFrameDecParams->decodeFrameInfo.dstPictureResource,
                                 currentDpbPictureResourceInfo,
                                 *pOutputPictureResource,
                                 *pOutputPictureResourceInfo,
                                 &frameSynchronizationInfo);
    }

    m_vkDevCtx->EndCommandBuffer(frameDataSlot.commandBuffer);

    const uint32_t waitSemaphoreMaxCount = 3;
    VkSemaphore waitSemaphores[waitSemaphoreMaxCount] = { VK_NULL_HANDLE };

    const uint32_t signalSemaphoreMaxCount = 3;
    VkSemaphore signalSemaphores[signalSemaphoreMaxCount] = { VK_NULL_HANDLE };

    uint32_t waitSemaphoreCount = 0;
    if (frameConsumerDoneSemaphore != VK_NULL_HANDLE) {
        waitSemaphores[waitSemaphoreCount] = frameConsumerDoneSemaphore;
        waitSemaphoreCount++;
    }

    uint32_t signalSemaphoreCount = 0;
    if (frameCompleteSemaphore != VK_NULL_HANDLE) {
        signalSemaphores[signalSemaphoreCount] = frameCompleteSemaphore;
        signalSemaphoreCount++;
    }

    uint64_t waitTlSemaphoresValues[waitSemaphoreMaxCount] = { 0 /* ignored for binary semaphores */ };
    uint64_t signalTlSemaphoresValues[signalSemaphoreMaxCount] = { 0 /* ignored for binary semaphores */ };
    VkTimelineSemaphoreSubmitInfo timelineSemaphoreInfos = {};
    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {

        if (m_dumpDecodeData) {
            uint64_t  currSemValue = 0;
            VkResult semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
            std::cout << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
	}

        waitSemaphores[waitSemaphoreCount] = m_hwLoadBalancingTimelineSemaphore;
        waitTlSemaphoresValues[waitSemaphoreCount] = m_decodePicCount - 1; // wait for the previous value to be signaled
        waitSemaphoreCount++;

        signalSemaphores[signalSemaphoreCount] = m_hwLoadBalancingTimelineSemaphore;
        signalTlSemaphoresValues[signalSemaphoreCount] = m_decodePicCount; // signal the current m_decodePicCount value
        signalSemaphoreCount++;

        timelineSemaphoreInfos.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineSemaphoreInfos.pNext = NULL;
        timelineSemaphoreInfos.waitSemaphoreValueCount = waitSemaphoreCount;
        timelineSemaphoreInfos.pWaitSemaphoreValues = waitTlSemaphoresValues;
        timelineSemaphoreInfos.signalSemaphoreValueCount = signalSemaphoreCount;
        timelineSemaphoreInfos.pSignalSemaphoreValues = signalTlSemaphoresValues;
        if (m_dumpDecodeData) {
	    std::cout << "\t Wait for: " << (waitSemaphoreCount ? waitTlSemaphoresValues[waitSemaphoreCount - 1] : 0) <<
                         ", signal at " << signalTlSemaphoresValues[signalSemaphoreCount - 1] << std::endl;
	}
    }

    assert(waitSemaphoreCount <= waitSemaphoreMaxCount);
    assert(signalSemaphoreCount <= signalSemaphoreMaxCount);

    VkPipelineStageFlags videoDecodeSubmitWaitStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
    submitInfo.pNext = (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) ? &timelineSemaphoreInfos : nullptr;
    submitInfo.waitSemaphoreCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = &videoDecodeSubmitWaitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frameDataSlot.commandBuffer;
    submitInfo.signalSemaphoreCount = signalSemaphoreCount;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (m_dumpDecodeData) {
        if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
            std::cout << "\t\t waitSemaphoreValueCount: " << timelineSemaphoreInfos.waitSemaphoreValueCount << std::endl;
            std::cout << "\t pWaitSemaphoreValues: " << timelineSemaphoreInfos.pWaitSemaphoreValues[0] << ", " <<
		                                        timelineSemaphoreInfos.pWaitSemaphoreValues[1] << ", " <<
						        timelineSemaphoreInfos.pWaitSemaphoreValues[2] << std::endl;
            std::cout << "\t\t signalSemaphoreValueCount: " << timelineSemaphoreInfos.signalSemaphoreValueCount << std::endl;
            std::cout << "\t pSignalSemaphoreValues: " << timelineSemaphoreInfos.pSignalSemaphoreValues[0] << ", " <<
		                                        timelineSemaphoreInfos.pSignalSemaphoreValues[1] << ", " <<
						        timelineSemaphoreInfos.pSignalSemaphoreValues[2] << std::endl;
        }

        std::cout << "\t waitSemaphoreCount: " << submitInfo.waitSemaphoreCount << std::endl;
        std::cout << "\t\t pWaitSemaphores: " << submitInfo.pWaitSemaphores[0] << ", " <<
	                                         submitInfo.pWaitSemaphores[1] << ", " <<
	                                         submitInfo.pWaitSemaphores[2] << std::endl;
        std::cout << "\t signalSemaphoreCount: " << submitInfo.signalSemaphoreCount << std::endl;
        std::cout << "\t\t pSignalSemaphores: " << submitInfo.pSignalSemaphores[0] << ", " <<
	                                         submitInfo.pSignalSemaphores[1] << ", " <<
					         submitInfo.pSignalSemaphores[2] << std::endl << std::endl;
    }

    if ((frameConsumerDoneSemaphore == VkSemaphore()) && (frameConsumerDoneFence != VkFence())) {
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameConsumerDoneFence, true, gFenceTimeout);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
    }

    result = m_vkDevCtx->ResetFences(*m_vkDevCtx, 1, &frameCompleteFence);
    assert(result == VK_SUCCESS);
    result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
    assert(result == VK_NOT_READY);

    result = m_vkDevCtx->MultiThreadedQueueSubmit(VulkanDeviceContext::DECODE, m_currentVideoQueueIndx,
                                                  1, &submitInfo, frameCompleteFence);
    assert(result == VK_SUCCESS);

    if (m_dumpDecodeData) {
        std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
        std::cout << "\t => Decode Submitted for CurrPicIdx: " << currPicIdx << std::endl
                  << "\t\tm_nPicNumInDecodeOrder: " << picNumInDecodeOrder << "\t\tframeCompleteFence " << frameCompleteFence
                  << "\t\tframeCompleteSemaphore " << frameCompleteSemaphore << "\t\tdstImageView "
                  << pCurrFrameDecParams->decodeFrameInfo.dstPictureResource.imageViewBinding << std::endl;
    }

    const bool checkDecodeIdleSync = false; // For fence/sync/idle debugging
    if (checkDecodeIdleSync) { // For fence/sync debugging
        if (frameCompleteFence == VkFence()) {
            result = m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_currentVideoQueueIndx);
            assert(result == VK_SUCCESS);
        } else {
            if (frameCompleteSemaphore == VkSemaphore()) {
                result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
                assert(result == VK_SUCCESS);
                result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
                assert(result == VK_SUCCESS);
            }
        }
    }

    if (m_dumpDecodeData && (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE)) { // For TL semaphore debug
       uint64_t  currSemValue = 0;
       VkResult semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
       std::cout << "\t TL semaphore value ater submit: " << currSemValue << ", status: " << semResult << std::endl;

       const bool waitOnTlSemaphore = false;
       if (waitOnTlSemaphore) {
           uint64_t value = m_decodePicCount;
           VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, VK_SEMAPHORE_WAIT_ANY_BIT, 1,
	                                    &m_hwLoadBalancingTimelineSemaphore, &value };
           std::cout << "\t TL semaphore wait for value: " << value << std::endl;
           semResult = m_vkDevCtx->WaitSemaphores(*m_vkDevCtx, &waitInfo, gLongTimeout);

           semResult = m_vkDevCtx->GetSemaphoreCounterValue(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, &currSemValue);
           std::cout << "\t TL semaphore value: " << currSemValue << ", status: " << semResult << std::endl;
       }
    }

    // For fence/sync debugging
    if (pDecodePictureInfo->flags.fieldPic) {
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
        assert(result == VK_SUCCESS);
    }

    const bool checkDecodeStatus = true; // Check the queries
    if (checkDecodeStatus && (frameSynchronizationInfo.queryPool != VK_NULL_HANDLE)) {
        VkQueryResultStatusKHR decodeStatus;
        result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx,
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

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
        m_currentVideoQueueIndx++;
        m_currentVideoQueueIndx %= m_vkDevCtx->GetVideoDecodeNumQueues();
    }
    m_decodePicCount++;
    return currPicIdx;
}

VkDeviceSize VkVideoDecoder::GetBitstreamBuffer(VkDeviceSize size,
                                          VkDeviceSize minBitstreamBufferOffsetAlignment,
                                          VkDeviceSize minBitstreamBufferSizeAlignment,
                                          const uint8_t* pInitializeBufferMemory,
                                          VkDeviceSize initializeBufferMemorySize,
                                          VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer)
{
    assert(initializeBufferMemorySize <= size);
    // size_t newSize = 4 * 1024 * 1024;
    VkDeviceSize newSize = size;
    assert(m_vkDevCtx);

    VkSharedBaseObj<VulkanBitstreamBufferImpl> newBitstreamBuffer;

    const bool enablePool = true;
    const bool debugBitstreamBufferDumpAlloc = false;
    int32_t availablePoolNode = -1;
    if (enablePool) {
        availablePoolNode = m_decodeFramesData.GetBitstreamBuffersQueue().GetAvailableNodeFromPool(newBitstreamBuffer);
    }
    if (!(availablePoolNode >= 0)) {
        VkResult result = VulkanBitstreamBufferImpl::Create(m_vkDevCtx,
                m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                newSize, minBitstreamBufferOffsetAlignment,
                minBitstreamBufferSizeAlignment,
                pInitializeBufferMemory, initializeBufferMemorySize, newBitstreamBuffer);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: VulkanBitstreamBufferImpl::Create() result: 0x%x\n", result);
            return 0;
        }
        if (debugBitstreamBufferDumpAlloc) {
            std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        }
        if (enablePool) {
            int32_t nodeAddedWithIndex = m_decodeFramesData.GetBitstreamBuffersQueue().AddNodeToPool(newBitstreamBuffer, true);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
            }
        }

    } else {

        assert(newBitstreamBuffer);
        newSize = newBitstreamBuffer->GetMaxSize();
        assert(initializeBufferMemorySize <= newSize);

        VkDeviceSize copySize = std::min<VkDeviceSize>(initializeBufferMemorySize, newSize);
        newBitstreamBuffer->CopyDataFromBuffer((const uint8_t*)pInitializeBufferMemory,
                                               0, // srcOffset
                                               0, // dstOffset
                                               copySize);

#ifdef CLEAR_BITSTREAM_BUFFERS_ON_CREATE
        newBitstreamBuffer->MemsetData(0x0, copySize, newSize - copySize);
#endif
        if (debugBitstreamBufferDumpAlloc) {
            std::cout << "\t\tFrom bitstream buffer pool with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;

            std::cout << "\t\t\t FreeNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetFreeNodesNumber();
            std::cout << " of MaxNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetMaxNodes();
            std::cout << ", AvailableNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetAvailableNodesNumber();
            std::cout << std::endl;
        }
    }
    bitstreamBuffer = newBitstreamBuffer;
    if (newSize > m_maxStreamBufferSize) {
        std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        m_maxStreamBufferSize = newSize;
    }
    return bitstreamBuffer->GetMaxSize();
}

VkResult VkVideoDecoder::Create(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                                int32_t videoQueueIndx,
                                bool useLinearOutput,
                                bool enableHwLoadBalancing,
                                int32_t numDecodeImagesInFlight,
                                int32_t numDecodeImagesToPreallocate,
                                int32_t numBitstreamBuffersToPreallocate,
                                VkSharedBaseObj<VkVideoDecoder>& vkVideoDecoder)
{
    VkSharedBaseObj<VkVideoDecoder> vkDecoder(new VkVideoDecoder(vkDevCtx,
                                                                 videoFrameBuffer,
                                                                 videoQueueIndx,
                                                                 useLinearOutput,
                                                                 enableHwLoadBalancing,
                                                                 numDecodeImagesInFlight,
                                                                 numBitstreamBuffersToPreallocate));
    if (vkDecoder) {
        vkVideoDecoder = vkDecoder;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void VkVideoDecoder::Deinitialize()
{
    if (m_vkDevCtx == nullptr) {
        return;
    }

    if (m_vkDevCtx->GetVideoDecodeNumQueues() > 1) {
        for (uint32_t queueId = 0; queueId <  (uint32_t)m_vkDevCtx->GetVideoDecodeNumQueues(); queueId++) {
            m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, queueId);
        }
    } else {
        m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_currentVideoQueueIndx);
    }

    if (m_hwLoadBalancingTimelineSemaphore != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_hwLoadBalancingTimelineSemaphore, NULL);
        m_hwLoadBalancingTimelineSemaphore = VK_NULL_HANDLE;
    }

    // m_vkDevCtx->DeviceWaitIdle();

    m_videoFrameBuffer = nullptr;
    m_decodeFramesData.deinit();
    m_videoSession = nullptr;
    m_vkDevCtx = nullptr;
}

VkVideoDecoder::~VkVideoDecoder()
{
    Deinitialize();
}

int32_t VkVideoDecoder::AddRef()
{
    return ++m_refCount;
}

int32_t VkVideoDecoder::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}
