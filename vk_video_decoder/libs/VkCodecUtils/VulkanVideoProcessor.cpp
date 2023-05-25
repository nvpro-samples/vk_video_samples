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

#include <assert.h>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "VulkanVideoProcessor.h"
#include "vulkan_interfaces.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

inline void CheckInputFile(const char* szInFilePath)
{
    std::ifstream fpIn(szInFilePath, std::ios::in | std::ios::binary);
    if (fpIn.fail()) {
        std::ostringstream err;
        err << "Unable to open input file: " << szInFilePath << std::endl;
        throw std::invalid_argument(err.str());
    }
}

int32_t VulkanVideoProcessor::Initialize(const VulkanDeviceContext* vkDevCtx,
                                         ProgramConfig& programConfig)
{

    const char* filePath = programConfig.videoFileName.c_str();
    int32_t videoQueueIndx =  programConfig.queueId;
    const char* outputFileName = (programConfig.outputFileName.size() == 0) ?
            nullptr : programConfig.outputFileName.c_str();
    const VkVideoCodecOperationFlagBitsKHR forceCodecType = programConfig.forceParserType;
    const bool enableStreamDemuxing = (programConfig.enableStreamDemuxing == 1);
    const int32_t defaultWidth = programConfig.initialWidth;
    const int32_t defaultHeight = programConfig.initialHeight;
    const int32_t defaultBitDepth = programConfig.initialBitdepth;
    const uint32_t loopCount = programConfig.loopCount;
    const uint32_t startFrame = 0;
    const int32_t  maxFrameCount = programConfig.maxFrameCount;
    const int32_t numDecodeImagesInFlight = std::max(programConfig.numDecodeImagesInFlight, 4);
    const int32_t numDecodeImagesToPreallocate = programConfig.numDecodeImagesToPreallocate;
    const int32_t numBitstreamBuffersToPreallocate = std::max(programConfig.numBitstreamBuffersToPreallocate, 4);
    const bool enableHwLoadBalancing = programConfig.enableHwLoadBalancing;
    const bool verbose = false;

    if (vkDevCtx->GetVideoDecodeQueue(videoQueueIndx) == VkQueue()) {
        std::cerr << "videoQueueIndx is out of bounds: " << videoQueueIndx <<
                     " Max decode queues: " << vkDevCtx->GetVideoDecodeNumQueues() << std::endl;
        assert(!"Invalid Video Queue");
        return -1;
    }

    Deinit();

    m_vkDevCtx = vkDevCtx;

    CheckInputFile(filePath);

    VkResult result = VideoStreamDemuxer::Create(filePath,
                                                 forceCodecType,
                                                 enableStreamDemuxing,
                                                 defaultWidth,
                                                 defaultHeight,
                                                 defaultBitDepth,
                                                 m_videoStreamDemuxer);

    if (result != VK_SUCCESS) {
        return -result;
    }

    m_usesStreamDemuxer = m_videoStreamDemuxer->IsStreamDemuxerEnabled();
    m_usesFramePreparser = m_videoStreamDemuxer->HasFramePreparser();

    if (verbose) {
        m_videoStreamDemuxer->DumpStreamParameters();
    }

    result =  VulkanVideoFrameBuffer::Create(vkDevCtx, m_vkVideoFrameBuffer);
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: Create VulkanVideoFrameBuffer result: 0x%x\n", result);
    }

    FILE* outFile = m_frameToFile.AttachFile(outputFileName);
    if ((outputFileName != nullptr) && (outFile == nullptr)) {
        fprintf( stderr, "Error opening the output file %s", outputFileName);
        return -1;
    }

    result = VkVideoDecoder::Create(vkDevCtx, m_vkVideoFrameBuffer,
                                    videoQueueIndx, (outFile != nullptr),
                                    enableHwLoadBalancing,
                                    numDecodeImagesInFlight,
                                    numDecodeImagesToPreallocate,
                                    numBitstreamBuffersToPreallocate,
                                    m_vkVideoDecoder);
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: Create VkVideoDecoder result: 0x%x\n", result);
    }

    VkVideoCoreProfile videoProfile(m_videoStreamDemuxer->GetVideoCodec(),
                                    m_videoStreamDemuxer->GetChromaSubsampling(),
                                    m_videoStreamDemuxer->GetLumaBitDepth(),
                                    m_videoStreamDemuxer->GetChromaBitDepth(),
                                    m_videoStreamDemuxer->GetProfileIdc());


#if HEADLESS_AV1
    if (VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR != m_videoStreamDemuxer->GetVideoCodec()) {
        assert(!"The video codec is not supported");
        return -1;
    }

    VkVideoCapabilitiesKHR videoCapabilities;
    videoCapabilities.minBitstreamBufferOffsetAlignment = 256;
    videoCapabilities.minBitstreamBufferSizeAlignment = 256;
#else
    if (!VulkanVideoCapabilities::IsCodecTypeSupported(vkDevCtx,
                                                       vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                                       m_videoStreamDemuxer->GetVideoCodec())) {
        std::cout << "*** The video codec " << VkVideoCoreProfile::CodecToName(m_videoStreamDemuxer->GetVideoCodec()) << " is not supported! ***" << std::endl;
        assert(!"The video codec is not supported");
        return -1;
    }

    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoDecodeCapabilitiesKHR videoDecodeCapabilities;
    result = VulkanVideoCapabilities::GetVideoDecodeCapabilities(m_vkDevCtx, videoProfile,
                                                                 videoCapabilities,
                                                                 videoDecodeCapabilities);

    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return -result;
    }
#endif

    const uint32_t defaultMinBufferSize = 2 * 1024 * 1024; // 2MB
    result = CreateParser(filePath,
                          m_videoStreamDemuxer->GetVideoCodec(),
                          defaultMinBufferSize,
                          (uint32_t)videoCapabilities.minBitstreamBufferOffsetAlignment,
                          (uint32_t)videoCapabilities.minBitstreamBufferSizeAlignment);
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: CreateParser() result: 0x%x\n", result);
    }

    m_loopCount = loopCount;
    m_startFrame = startFrame;
    m_maxFrameCount = maxFrameCount;

    return 0;
}

VkResult VulkanVideoProcessor::Create(const VulkanDeviceContext* vkDevCtx,
                                      VkSharedBaseObj<VulkanVideoProcessor>& vulkanVideoProcessor)
{
    VkSharedBaseObj<VulkanVideoProcessor> videoProcessor(new VulkanVideoProcessor(vkDevCtx));

    if (videoProcessor) {
        vulkanVideoProcessor = videoProcessor;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkFormat VulkanVideoProcessor::GetFrameImageFormat(int32_t* pWidth, int32_t* pHeight, int32_t* pBitDepth)
{
    VkFormat frameImageFormat = VK_FORMAT_UNDEFINED;
    if (m_videoStreamDemuxer) {
        if (m_videoStreamDemuxer->GetBitDepth() == 8) {
            frameImageFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        } else if (m_videoStreamDemuxer->GetBitDepth() == 10) {
            frameImageFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        } else if (m_videoStreamDemuxer->GetBitDepth() == 12) {
            frameImageFormat = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        } else {
            assert(0);
        }

        if (pWidth) {
            *pWidth = m_videoStreamDemuxer->GetWidth();
        }

        if (pHeight) {
            *pHeight = m_videoStreamDemuxer->GetHeight();
        }

        if (pBitDepth) {
            *pBitDepth = m_videoStreamDemuxer->GetBitDepth();
        }
    }

    return frameImageFormat;
}

int32_t VulkanVideoProcessor::GetWidth()
{
    return m_videoStreamDemuxer->GetWidth();
}

int32_t VulkanVideoProcessor::GetHeight()
{
    return m_videoStreamDemuxer->GetHeight();
}

int32_t VulkanVideoProcessor::GetBitDepth()
{
    return m_videoStreamDemuxer->GetBitDepth();
}

void VulkanVideoProcessor::Deinit()
{

    m_vkParser = nullptr;
    m_vkVideoDecoder = nullptr;
    m_vkVideoFrameBuffer = nullptr;
    m_videoStreamDemuxer = nullptr;
}

void VulkanVideoProcessor::DumpVideoFormat(const VkParserDetectedVideoFormat* videoFormat, bool dumpData)
{
    if (dumpData) {
        std::cout << "Display Area : " << std::endl
                  << "\tLeft : " << videoFormat->display_area.left << std::endl
                  << "\tRight : " << videoFormat->display_area.right << std::endl
                  << "\tTop : " << videoFormat->display_area.top << std::endl
                  << "\tBottom : " << videoFormat->display_area.bottom << std::endl;
    }

    if (dumpData) {
        std::cout << "Geometry  : " << std::endl
                  << "\tCoded Width : " << videoFormat->coded_width << std::endl
                  << "\tDisplayed Width : " << videoFormat->display_area.right - videoFormat->display_area.left << std::endl
                  << "\tCoded Height : " << videoFormat->coded_height << std::endl
                  << "\tDisplayed Height : " << videoFormat->display_area.bottom - videoFormat->display_area.top << std::endl;
    }

    const char* pCodec = VkVideoCoreProfile::CodecToName(videoFormat->codec);
    if (dumpData) {
        std::cout << "Codec : " << pCodec << std::endl;
    }

    /* These below token numbers are based on "chroma_format_idc" from the spec. */
    /* Also, mind the separate_colour_plane_flag, as well. */
    static const char* nvVideoChromaFormat[] = {
        nullptr,
        "Monochrome",
        "420",
        nullptr,
        "422",
        nullptr,
        nullptr,
        nullptr,
        "444",
    };
    assert(videoFormat->chromaSubsampling < sizeof(nvVideoChromaFormat)/sizeof(nvVideoChromaFormat[0]));
    assert(nvVideoChromaFormat[videoFormat->chromaSubsampling] != nullptr);
    const char* pVideoChromaFormat = nvVideoChromaFormat[videoFormat->chromaSubsampling];
    if (dumpData) {
        std::cout << "VideoChromaFormat : " << pVideoChromaFormat << std::endl;
    }

    static const char* VideoFormat[] = {
        // Definitions for video_format

        "Component",
        "PAL",
        "NTSC",
        "SECAM",
        "MAC",
        "Unspecified",
        "Reserved6",
        "Reserved7",
    };
    assert(videoFormat->video_signal_description.video_format < sizeof(VideoFormat)/sizeof(VideoFormat[0]));
    const char* pVideoFormat = VideoFormat[videoFormat->video_signal_description.video_format];
    if (dumpData) {
        std::cout << "VideoFormat : " << pVideoFormat << std::endl;
    }

    const char* ColorPrimaries[] = {
        // Definitions for color_primaries

        "Forbidden",
        "BT709",
        "Unspecified",
        "Reserved",
        "BT470M",
        "BT470BG",
        "SMPTE170M",
        "SMPTE240M",
        "GenericFilm",
        "BT2020",
    };
    assert(videoFormat->video_signal_description.color_primaries < sizeof(ColorPrimaries)/sizeof(ColorPrimaries[0]));
    const char* pColorPrimaries = ColorPrimaries[videoFormat->video_signal_description.color_primaries];
    if (dumpData) {
        std::cout << "ColorPrimaries : " << pColorPrimaries << std::endl;
    }

    const char* TransferCharacteristics[] = {
        "Forbidden",
        "BT709",
        "Unspecified",
        "Reserved",
        "BT470M",
        "BT470BG",
        "SMPTE170M",
        "SMPTE240M",
        "Linear",
        "Log100",
        "Log316",
        "IEC61966_2_4",
        "BT1361",
        "IEC61966_2_1",
        "BT2020",
        "BT2020_2",
        "ST2084",
        "ST428_1",
    };
    assert(videoFormat->video_signal_description.transfer_characteristics < sizeof(TransferCharacteristics)/sizeof(TransferCharacteristics[0]));
    const char* pTransferCharacteristics = TransferCharacteristics[videoFormat->video_signal_description.transfer_characteristics];
    if (dumpData) {
        std::cout << "TransferCharacteristics : " << pTransferCharacteristics << std::endl;
    }

    const char* MatrixCoefficients[] = {
        "Forbidden",
        "BT709",
        "Unspecified",
        "Reserved",
        "FCC",
        "BT470BG",
        "SMPTE170M",
        "SMPTE240M",
        "YCgCo",
        "BT2020_NCL",
        "BT2020_CL",
    };
    assert(videoFormat->video_signal_description.matrix_coefficients < sizeof(MatrixCoefficients)/sizeof(MatrixCoefficients[0]));
    const char* pMatrixCoefficients = MatrixCoefficients[videoFormat->video_signal_description.matrix_coefficients];
    if (dumpData) {
        std::cout << "MatrixCoefficients : " << pMatrixCoefficients << std::endl;
    }
}

const VkMpFormatInfo* YcbcrVkFormatInfo(const VkFormat format);

size_t VulkanVideoProcessor::ConvertFrameToNv12(DecodedFrame* pFrame,
                                                VkSharedBaseObj<VkImageResource>& imageResource,
                                                uint8_t* pOutBuffer, size_t bufferSize)
{
    size_t outputBufferSize = 0;
    VkResult result = VK_SUCCESS;

    VkDevice device   = imageResource->GetDevice();
    VkImage  srcImage = imageResource->GetImage ();
    VkFormat format   = imageResource->GetImageCreateInfo().format;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> srcImageDeviceMemory(imageResource->GetMemory());

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
    assert(pFrame->frameCompleteFence != VK_NULL_HANDLE);
    const uint64_t fenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;
    int32_t retryCount = 5;
    do {
        result = m_vkDevCtx->WaitForFences(device, 1, &pFrame->frameCompleteFence, VK_TRUE, fenceTimeout);
        if ((result != VK_SUCCESS) && (pFrame->queryPool != VK_NULL_HANDLE)) {
            std::cout << "WaitForFences timeout " << fenceTimeout
                    << " result " << result << " retry " << retryCount << std::endl << std::flush;

            VkQueryResultStatusKHR decodeStatus;
            result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx,
                                                     pFrame->queryPool,
                                                     pFrame->startQueryId,
                                                     1,
                                                     sizeof(decodeStatus),
                                                     &decodeStatus,
                                                     sizeof(decodeStatus),
                                                     VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);

            if (result != VK_SUCCESS) {
                printf("\nERROR: GetQueryPoolResults() result: 0x%x\n", result);
            }

            std::cout << "\t +++++++++++++++++++++++++++< " << (pFrame ? pFrame->pictureIndex : -1)
                    << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << (pFrame ? pFrame->pictureIndex : -1) << std::endl
                    << "\t\tdecodeStatus: " << decodeStatus << std::endl;
        }
        retryCount--;
    } while ((result == VK_TIMEOUT) && (retryCount > 0));

    // Map the image and read the image data.
    VkDeviceSize imageOffset = imageResource->GetImageDeviceMemoryOffset();
    VkDeviceSize maxSize = 0;
    const uint8_t* readImagePtr = srcImageDeviceMemory->GetReadOnlyDataPtr(imageOffset, maxSize);
    assert(readImagePtr != nullptr);

    int secondaryPlaneHeight = pFrame->displayHeight;
    int imageHeight = pFrame->displayHeight;
    bool isUnnormalizedRgba = false;
    if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
        isUnnormalizedRgba = true;
    }

    if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY) {
        secondaryPlaneHeight /= 2;
    }

    VkImageSubresource subResource = {};
    VkSubresourceLayout layouts[3];
    memset(layouts, 0x00, sizeof(layouts));

    if (mpInfo && !isUnnormalizedRgba) {
        switch (mpInfo->planesLayout.layout) {
            case YCBCR_SINGLE_PLANE_UNNORMALIZED:
            case YCBCR_SINGLE_PLANE_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                break;
            case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[1]);
                break;
            case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
            case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
            case YCBCR_PLANAR_STRIDE_PADDED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[1]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[2]);
                break;
            default:
                assert(0);
        }

    } else {
        m_vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
    }

    // Treat all non 8bpp formats as 16bpp for output to prevent any loss.
    uint32_t bytesPerPixel = 1;
    if (mpInfo->planesLayout.bpp != YCBCRA_8BPP) {
        bytesPerPixel = 2;
    }

    uint32_t numPlanes = 3;
    VkSubresourceLayout yuvPlaneLayouts[3] = {};
    yuvPlaneLayouts[0].offset = 0;
    yuvPlaneLayouts[0].rowPitch = pFrame->displayWidth * bytesPerPixel;
    yuvPlaneLayouts[1].offset = yuvPlaneLayouts[0].rowPitch * pFrame->displayHeight;
    yuvPlaneLayouts[1].rowPitch = pFrame->displayWidth * bytesPerPixel;
    if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX) {
        yuvPlaneLayouts[1].rowPitch /= 2;
    }
    yuvPlaneLayouts[2].offset = yuvPlaneLayouts[1].offset + (yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
    yuvPlaneLayouts[2].rowPitch = pFrame->displayWidth * bytesPerPixel;
    if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX) {
        yuvPlaneLayouts[2].rowPitch /= 2;
    }

    // Copy the luma plane, always assume the 422 or 444 formats and src CbCr always is interleaved (shares the same plane).
    uint32_t numCompatiblePlanes = 1;
    for (uint32_t plane = 0; plane < numCompatiblePlanes; plane++) {
        const uint8_t* pSrc = readImagePtr + layouts[plane].offset;
        uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
        for (int height = 0; height < imageHeight; height++) {
            memcpy(pDst, pSrc, (size_t)yuvPlaneLayouts[plane].rowPitch);
            pDst += (size_t)yuvPlaneLayouts[plane].rowPitch;
            pSrc += (size_t)layouts[plane].rowPitch;
        }
    }

    // Copy the chroma plane(s)
    if (mpInfo->planesLayout.bpp == YCBCRA_10BPP) {
        assert(format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16);

        // In order to compare results in Fluster, convert VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 to YUV420P10. The first step is to deinterleave the planes.
        // Rotation of the 16-bit words happens at the end of this function.
        const uint8_t* pSrc = readImagePtr + layouts[1].offset;
        const uint16_t *pSrc16 = (uint16_t *)pSrc;
        uint16_t *pCbDst16 = (uint16_t *)(pOutBuffer + yuvPlaneLayouts[1].offset);
        uint16_t *pCrDst16 = (uint16_t *)(pOutBuffer + yuvPlaneLayouts[2].offset);

        // assert(((layouts[1].rowPitch / 2) * secondaryPlaneHeight) < std::numeric_limits<uint16_t>::max());
        for (int height = 0; height < secondaryPlaneHeight; height++)
        {
            VkDeviceSize samplesInRow = yuvPlaneLayouts[1].rowPitch;
            VkDeviceSize interleavedSamplesInRow = samplesInRow / 2;
            for (VkDeviceSize interleavedSampleIdx = 0; interleavedSampleIdx < interleavedSamplesInRow; interleavedSampleIdx++)
            {
                *pCbDst16++ = pSrc16[2 * interleavedSampleIdx];
                *pCrDst16++ = pSrc16[2 * interleavedSampleIdx + 1];
            }
            pSrc16 += layouts[1].rowPitch / 2;
        }


        if (false)
        {
            // Sometimes the driver reports .size = 0 (but a correct rowPictch), so this simplisitic approach doesn't work always..
            VkDeviceSize numWords = layouts[1].size / (2 * sizeof(uint16_t));

            for (size_t i = 0; i < numWords; i++) {
                *pCbDst16++ = *pSrc16;
                pSrc16 += 2;
            }

            pSrc16 = (uint16_t *)pSrc;
            pSrc16++;
            for (size_t i = 0; i < numWords; i++) {
                *pCrDst16++ = *pSrc16;
                pSrc16 += 2;
            }
        }
        //memcpy(pDst, pSrc, layouts[1].size);
    } else {
        assert(mpInfo->planesLayout.bpp == YCBCRA_8BPP);
        assert(format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
        for (uint32_t plane = numCompatiblePlanes; plane < numPlanes; plane++) {
            uint32_t srcPlane = std::min(plane, mpInfo->planesLayout.numberOfExtraPlanes);
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
            for (int height = 0; height < secondaryPlaneHeight; height++) {
                const uint8_t* pSrc;
                if (srcPlane != plane) {
                    pSrc = readImagePtr + layouts[srcPlane].offset + ((plane - 1) * bytesPerPixel) + (layouts[srcPlane].rowPitch * height);

                } else {
                    pSrc = readImagePtr + layouts[srcPlane].offset + (layouts[srcPlane].rowPitch * height);
                }

                for (VkDeviceSize width = 0; width < (yuvPlaneLayouts[plane].rowPitch / bytesPerPixel); width++) {
                    memcpy(pDst, pSrc, bytesPerPixel);
                    pDst += bytesPerPixel;
                    pSrc += 2 * bytesPerPixel;
                }
            }
        }
    }

    outputBufferSize += ((size_t)yuvPlaneLayouts[0].rowPitch * imageHeight);
    if (mpInfo->planesLayout.numberOfExtraPlanes >= 1) {
        outputBufferSize += ((size_t)yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
        outputBufferSize += ((size_t)yuvPlaneLayouts[2].rowPitch * secondaryPlaneHeight);
    }

    if (mpInfo->planesLayout.bpp == YCBCRA_10BPP) {
        // Change from p010 packing format to raw YUV format, this is for fluster validation.
        assert(format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16);
        uint16_t *converter = (uint16_t *)(pOutBuffer + yuvPlaneLayouts[0].offset);
        size_t wordsToConvert = outputBufferSize / sizeof(uint16_t);
        while (wordsToConvert--)
        {
            uint16_t sample = *converter;
            *converter = ((0x3F & sample) << 10) | ((0xFFC0 & sample) >> 6); // ror 6
            converter++;
        }
    }

    return outputBufferSize;
}

size_t VulkanVideoProcessor::OutputFrameToFile(DecodedFrame* pFrame)
{
    if (!m_frameToFile) {
        return (size_t)-1;
    }

    assert(pFrame != nullptr);
    assert(!!pFrame->outputImageView);
    assert(pFrame->pictureIndex != -1);

    VkSharedBaseObj<VkImageResource> imageResource = pFrame->outputImageView->GetImageResource();
    uint8_t* pLinearMemory = m_frameToFile.EnsureAllocation(m_vkDevCtx, imageResource);
    assert(pLinearMemory != nullptr);

    // Needed allocation size can shrink, but may never grow. Frames will be allocated for maximum resolution upfront.
    assert((pFrame->displayWidth >= 0) && (pFrame->displayHeight >= 0));

    // Convert frame to linear image format.
    size_t usedBufferSize = ConvertFrameToNv12(pFrame,
                                               imageResource,
                                               pLinearMemory,
                                               m_frameToFile.GetMaxFrameSize());

    // Write image to file.
    return m_frameToFile.WriteDataToFile(0, usedBufferSize);
}

void VulkanVideoProcessor::Restart(void)
{
    m_videoStreamDemuxer->Rewind();
    m_videoFrameNum = false;
    m_currentBitstreamOffset = 0;
}

bool VulkanVideoProcessor::StreamCompleted()
{
    if (--m_loopCount > 0) {
        std::cout << "Restarting video stream with loop number " << (m_loopCount + 1) << std::endl;
        // Reload the file stream
        Restart();
        return false;
    } else {
        std::cout << "End of Video Stream with status  " << VK_SUCCESS << std::endl;
        return true;
    }
}

int32_t VulkanVideoProcessor::ParserProcessNextDataChunk()
{
    if (m_videoStreamsCompleted) {
        return -1;
    }

    int32_t retValue = 0;
    int64_t bitstreamChunkSize = 0;
    size_t  bitstreamBytesConsumed = 0;
    const uint8_t* pBitstreamData = nullptr;
    bool requiresPartialParsing = false;
    if (m_usesFramePreparser || m_usesStreamDemuxer) {
        bitstreamChunkSize = m_videoStreamDemuxer->DemuxFrame(&pBitstreamData);
        assert(bitstreamBytesConsumed <= (size_t)std::numeric_limits<int32_t>::max());
        retValue = (int32_t)bitstreamChunkSize;
    } else {
        bitstreamChunkSize = m_videoStreamDemuxer->ReadBitstreamData(&pBitstreamData, m_currentBitstreamOffset);
        requiresPartialParsing = true;
    }
    const bool bitstreamHasMoreData = ((bitstreamChunkSize > 0) && (pBitstreamData != nullptr));
    if (bitstreamHasMoreData) {
        assert((uint64_t)bitstreamChunkSize < (uint64_t)std::numeric_limits<size_t>::max());
        VkResult parserStatus = ParseVideoStreamData(pBitstreamData, (size_t)bitstreamChunkSize,
                                                     &bitstreamBytesConsumed,
                                                     requiresPartialParsing);
        if (parserStatus != VK_SUCCESS) {
            m_videoStreamsCompleted = true;
            std::cerr << "Parser: end of Video Stream with status  " << parserStatus << std::endl;
            retValue = -1;
        } else {
            retValue = (int32_t)bitstreamBytesConsumed;
        }
        assert(bitstreamBytesConsumed <= (size_t)std::numeric_limits<int32_t>::max());
        m_currentBitstreamOffset += bitstreamBytesConsumed;
    } else {
        // Call the parser one last time with zero buffer to flush the display queue.
        ParseVideoStreamData(nullptr, 0, &bitstreamBytesConsumed, requiresPartialParsing);
        m_videoStreamsCompleted = StreamCompleted();
        retValue = 0;
    }

    return retValue;
}

int32_t VulkanVideoProcessor::GetNextFrame(DecodedFrame* pFrame, bool* endOfStream)
{
    // The below call to DequeueDecodedPicture allows returning the next frame without parsing of the stream.
    // Parsing is only done when there are no more frames in the queue.
    int32_t framesInQueue = m_vkVideoFrameBuffer->DequeueDecodedPicture(pFrame);

    // Loop until a frame (or more) is parsed and added to the queue.
    while ((framesInQueue == 0) && !m_videoStreamsCompleted) {

        ParserProcessNextDataChunk();

        framesInQueue = m_vkVideoFrameBuffer->DequeueDecodedPicture(pFrame);
    }

    if (framesInQueue) {

        if (m_videoFrameNum == 0) {
            DumpVideoFormat(m_vkVideoDecoder->GetVideoFormatInfo(), false);
        }

        if (m_frameToFile) {
            OutputFrameToFile(pFrame);
        }

        m_videoFrameNum++;
    }

    if ((m_maxFrameCount != -1) && (m_videoFrameNum >= (uint32_t)m_maxFrameCount)) {
        // Tell the FrameProcessor we're done after this frame is drawn.
        std::cout << "Number of video frames " << m_videoFrameNum
                  << " of max frame number " << m_maxFrameCount << std::endl;
        m_videoStreamsCompleted = StreamCompleted();
        *endOfStream = m_videoStreamsCompleted;
        return -1;
    }

    *endOfStream = m_videoStreamsCompleted;

    if ((framesInQueue == 0) && m_videoStreamsCompleted) {
        return -1;
    }

    return 1;
}

int32_t VulkanVideoProcessor::ReleaseDisplayedFrame(DecodedFrame* pDisplayedFrame)
{
    if (pDisplayedFrame->pictureIndex != -1) {
        DecodedFrameRelease decodedFramesRelease = { pDisplayedFrame->pictureIndex };
        DecodedFrameRelease* decodedFramesReleasePtr = &decodedFramesRelease;

        pDisplayedFrame->pictureIndex = -1;

        decodedFramesRelease.decodeOrder = pDisplayedFrame->decodeOrder;
        decodedFramesRelease.displayOrder = pDisplayedFrame->displayOrder;

        decodedFramesRelease.hasConsummerSignalFence = pDisplayedFrame->hasConsummerSignalFence;
        decodedFramesRelease.hasConsummerSignalSemaphore = pDisplayedFrame->hasConsummerSignalSemaphore;
        decodedFramesRelease.timestamp = 0;

        return m_vkVideoFrameBuffer->ReleaseDisplayedPicture(&decodedFramesReleasePtr, 1);
    }

    return -1;
}

VkResult VulkanVideoProcessor::CreateParser(const char* filename,
                                            VkVideoCodecOperationFlagBitsKHR vkCodecType,
                                            uint32_t defaultMinBufferSize,
                                            uint32_t bufferOffsetAlignment,
                                            uint32_t bufferSizeAlignment)
{
    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION };
    static const VkExtensionProperties h265StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION };
#ifdef ENABLE_AV1_DECODER
    static const VkExtensionProperties av1StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION };
#endif

    const VkExtensionProperties* pStdExtensionVersion = NULL;
    if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
        pStdExtensionVersion = &h264StdExtensionVersion;
    } else if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
        pStdExtensionVersion = &h265StdExtensionVersion;
#ifdef ENABLE_AV1_DECODER
    } else if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
        pStdExtensionVersion = &av1StdExtensionVersion;
#endif
    } else {
        assert(!"Unsupported Codec Type");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkSharedBaseObj<IVulkanVideoDecoderHandler> decoderHandler(m_vkVideoDecoder);
    VkSharedBaseObj<IVulkanVideoFrameBufferParserCb> videoFrameBufferCb(m_vkVideoFrameBuffer);
    return vulkanCreateVideoParser(decoderHandler,
                                   videoFrameBufferCb,
                                   vkCodecType,
                                   pStdExtensionVersion,
                                   1, // maxNumDecodeSurfaces - currently ignored
                                   1, // maxNumDpbSurfaces - currently ignored
                                   defaultMinBufferSize,
                                   bufferOffsetAlignment,
                                   bufferSizeAlignment,
                                   0, // clockRate - default 0 = 10Mhz
                                   m_vkParser);
}

VkResult VulkanVideoProcessor::ParseVideoStreamData(const uint8_t* pData, size_t size,
                                                    size_t *pnVideoBytes, bool doPartialParsing,
                                                    uint32_t flags, int64_t timestamp) {
    if (!m_vkParser) {
        assert(!"Parser not initialized!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkParserSourceDataPacket packet = { 0 };
    packet.payload = pData;
    packet.payload_size = size;
    packet.flags = flags;
    if (timestamp) {
        packet.flags |= VK_PARSER_PKT_TIMESTAMP;
    }
    packet.timestamp = timestamp;
    if (!pData || size == 0) {
        packet.flags |= VK_PARSER_PKT_ENDOFSTREAM;
    }

    return m_vkParser->ParseVideoData(&packet, pnVideoBytes, doPartialParsing);
}
