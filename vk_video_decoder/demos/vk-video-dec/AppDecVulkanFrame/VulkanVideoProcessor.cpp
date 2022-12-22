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
#include "VkCodecUtils/VulkanVideoUtils.h"
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

int32_t VulkanVideoProcessor::Init(const VulkanDecodeContext* vulkanDecodeContext,
                                   vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo,
                                   const char* filePath,
                                   const char* outputFileName,
                                   int forceParserType)
{
    Deinit();

    try {
        CheckInputFile(filePath);

        m_pFFmpegDemuxer = new FFmpegDemuxer(filePath);
        if (m_pFFmpegDemuxer == NULL) {
            return -VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        m_pFFmpegDemuxer->DumpStreamParameters();
    } catch (const std::exception& ex) {
        std::cout << ex.what();
        exit(1);
    }

    m_pVideoFrameBuffer = VulkanVideoFrameBuffer::CreateInstance(pVideoRendererDeviceInfo);
    assert(m_pVideoFrameBuffer);
    if (m_pVideoFrameBuffer == NULL) {
        return -VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    FILE* outFile = m_frameToFile.AttachFile(outputFileName);
    if ((outputFileName != nullptr) && (outFile == nullptr)) {
        fprintf( stderr, "Error opening the output file %s", outputFileName);
        return -1;
    }
    m_pDecoder = new NvVkDecoder(vulkanDecodeContext, m_pVideoFrameBuffer, (outFile != nullptr));
    if (m_pDecoder == NULL) {
        return -VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult result = CreateParser(m_pFFmpegDemuxer, filePath, FFmpeg2NvCodecId(m_pFFmpegDemuxer->GetVideoCodec()));
    assert(result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "\nERROR: CreateParser() result: 0x%x\n", result);
    }

    return 0;
}

VkFormat VulkanVideoProcessor::GetFrameImageFormat(int32_t* pWidth, int32_t* pHeight, int32_t* pBitDepth)
{
    VkFormat frameImageFormat = VK_FORMAT_UNDEFINED;
    if (m_pFFmpegDemuxer) {
        if (m_pFFmpegDemuxer->GetBitDepth() == 8) {
            frameImageFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        } else if (m_pFFmpegDemuxer->GetBitDepth() == 10) {
            frameImageFormat = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        } else if (m_pFFmpegDemuxer->GetBitDepth() == 12) {
            frameImageFormat = VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        } else {
            assert(0);
        }

        if (pWidth) {
            *pWidth = m_pFFmpegDemuxer->GetWidth();
        }

        if (pHeight) {
            *pHeight = m_pFFmpegDemuxer->GetHeight();
        }

        if (pBitDepth) {
            *pBitDepth = m_pFFmpegDemuxer->GetBitDepth();
        }
    }

    return frameImageFormat;
}

int32_t VulkanVideoProcessor::GetWidth()
{
    return m_pFFmpegDemuxer->GetWidth();
}

int32_t VulkanVideoProcessor::GetHeight()
{
    return m_pFFmpegDemuxer->GetHeight();
}

int32_t VulkanVideoProcessor::GetBitDepth()
{
    return m_pFFmpegDemuxer->GetBitDepth();
}

void VulkanVideoProcessor::Deinit()
{

    if (m_pParser) {
        m_pParser->Release();
        m_pParser = NULL;
    }

    if (m_pDecoder) {
        delete m_pDecoder;
        m_pDecoder = NULL;
    }

    if (m_pVideoFrameBuffer) {
        m_pVideoFrameBuffer->Release();
        m_pVideoFrameBuffer = NULL;
    }

    if (m_pFFmpegDemuxer) {
        delete m_pFFmpegDemuxer;
        m_pFFmpegDemuxer = NULL;
    }
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

    const char* pCodec = NvVideoProfile::CodecToName(videoFormat->codec);
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
                                                VkDevice device, VkImage outputImage,
                                                VkDeviceMemory imageDeviceMemory, VkFormat format,
                                                uint8_t* pOutBuffer, size_t bufferSize)
{
    size_t outputBufferSize = 0;
    VkResult result = VK_SUCCESS;

    // Bind memory for the image.
    VkMemoryRequirements memReqs = {};
    vk::GetImageMemoryRequirements(device, outputImage, &memReqs);
    if (bufferSize < memReqs.size) {
        return 0;
    }

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
    assert(pFrame->frameCompleteFence != VK_NULL_HANDLE);
    const uint64_t fenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;
    int32_t retryCount = 5;
    do {
        result = vk::WaitForFences(device, 1, &pFrame->frameCompleteFence, VK_TRUE, fenceTimeout);
        if (result != VK_SUCCESS) {
            std::cout << "WaitForFences timeout " << fenceTimeout
                    << " result " << result << " retry " << retryCount << std::endl << std::flush;
        }
        retryCount--;
    } while ((result == VK_TIMEOUT) && (retryCount > 0));

    // Map the image and read the image data.
    uint8_t* ptr = NULL;
    size_t size = (size_t)memReqs.size;
    VkResult memMapResult = vk::MapMemory(device, imageDeviceMemory, 0, size, 0, (void**)&ptr);
    if (memMapResult != VK_SUCCESS) {
        return 0;
    }

    assert(ptr != nullptr);

    const VkMappedMemoryRange range = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,  // sType
        NULL,                                   // pNext
        imageDeviceMemory,                      // memory
        0,                                      // offset
        size,                                   // size
    };

    if (((memReqs.memoryTypeBits & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) &&
        ((memReqs.memoryTypeBits & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)) {

        result = vk::FlushMappedMemoryRanges(device, 1u, &range);
    }

    {
        VkImageSubresource subResource = {};
        VkSubresourceLayout layouts[3];

        memset(layouts, 0x00, sizeof(layouts));
        int secondaryPlaneHeight = pFrame->displayHeight;
        int imageHeight = pFrame->displayHeight;
        bool isUnnormalizedRgba = false;
        if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
            isUnnormalizedRgba = true;
        }

        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY) {
            secondaryPlaneHeight /= 2;
        }

        if (mpInfo && !isUnnormalizedRgba) {
            switch (mpInfo->planesLayout.layout) {
                case YCBCR_SINGLE_PLANE_UNNORMALIZED:
                case YCBCR_SINGLE_PLANE_INTERLEAVED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[0]);
                    break;
                case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[0]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[1]);
                    break;
                case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
                case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
                case YCBCR_PLANAR_STRIDE_PADDED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[0]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[1]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                    vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[2]);
                    break;
                default:
                    assert(0);
            }

        } else {
            vk::GetImageSubresourceLayout(device, outputImage, &subResource, &layouts[0]);
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
            const uint8_t* pSrc = ptr + layouts[plane].offset;
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
            for (int height = 0; height < imageHeight; height++) {
                memcpy(pDst, pSrc, (size_t)yuvPlaneLayouts[plane].rowPitch);
                pDst += (size_t)yuvPlaneLayouts[plane].rowPitch;
                pSrc += (size_t)layouts[plane].rowPitch;
            }
        }

        // Copy the chroma plane(s)
        for (uint32_t plane = numCompatiblePlanes; plane < numPlanes; plane++) {
            uint32_t srcPlane = std::min(plane, mpInfo->planesLayout.numberOfExtraPlanes);
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
            for (int height = 0; height < secondaryPlaneHeight; height++) {
                const uint8_t* pSrc;
                if (srcPlane != plane) {
                    pSrc = ptr + layouts[srcPlane].offset + ((plane - 1) * bytesPerPixel) + (layouts[srcPlane].rowPitch * height);

                } else {
                    pSrc = ptr + layouts[srcPlane].offset + (layouts[srcPlane].rowPitch * height);
                }

                for (VkDeviceSize width = 0; width < (yuvPlaneLayouts[plane].rowPitch / bytesPerPixel); width++) {
                    memcpy(pDst, pSrc, bytesPerPixel);
                    pDst += bytesPerPixel;
                    pSrc += 2 * bytesPerPixel;
                }
            }
        }

        outputBufferSize += ((size_t)yuvPlaneLayouts[0].rowPitch * imageHeight);
        if (mpInfo->planesLayout.numberOfExtraPlanes >= 1) {
            outputBufferSize += ((size_t)yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
            outputBufferSize += ((size_t)yuvPlaneLayouts[2].rowPitch * secondaryPlaneHeight);
        }
    }

    vk::UnmapMemory(device, imageDeviceMemory);
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
    VkDevice device = pFrame->outputImageView->GetDevice();
    VkImage outputImage = imageResource->GetImage();
    VkDeviceMemory imageDeviceMemory = imageResource->GetMemory();
    VkFormat format = pFrame->outputImageView->GetImageResource()->GetImageCreateInfo().format;

    uint8_t* pLinearMemory = m_frameToFile.EnsureAllocation(device, outputImage, format,
                                                            pFrame->displayWidth,
                                                            pFrame->displayHeight);
    assert(pLinearMemory != nullptr);

    // Needed allocation size can shrink, but may never grow. Frames will be allocated for maximum resolution upfront.
    assert((pFrame->displayWidth >= 0) && (pFrame->displayHeight >= 0) &&
        (pFrame->displayWidth <= m_frameToFile.GetMaxWidth()) &&
        (pFrame->displayHeight <= m_frameToFile.GetMaxHeight()));

    // Convert frame to linear image format.
    size_t usedBufferSize = ConvertFrameToNv12(pFrame, device,
                                               outputImage, imageDeviceMemory, format,
                                               pLinearMemory, m_frameToFile.GetMaxFrameSize());

    // Write image to file.
    return m_frameToFile.WriteDataToFile(0, usedBufferSize);
}

void VulkanVideoProcessor::Restart(void)
{
    m_pFFmpegDemuxer->Rewind();
    m_videoStreamHasEnded = false;
}

int32_t VulkanVideoProcessor::GetNextFrames(DecodedFrame* pFrame, bool* endOfStream)
{
    const bool doPartialParsing = false;

    int32_t nVideoBytes = 0, framesInQueue = 0;

    // The below call to DequeueDecodedPicture allows returning the next frame without parsing of the stream.
    // Parsing is only done when there are no more frames in the queue.
    framesInQueue = m_pVideoFrameBuffer->DequeueDecodedPicture(pFrame);

    // Loop until a frame (or more) is parsed and added to the queue.
    while ((framesInQueue == 0) && !m_videoStreamHasEnded) {
        if (!m_videoStreamHasEnded) {
            m_pFFmpegDemuxer->Demux(&m_pBitStreamVideo, &nVideoBytes);
            VkResult parserStatus = VK_ERROR_DEVICE_LOST;
            parserStatus = ParseVideoStreamData(m_pBitStreamVideo, nVideoBytes, &nVideoBytes, doPartialParsing);
            if (parserStatus != VK_SUCCESS || nVideoBytes == 0) {
                m_videoStreamHasEnded = true;
                std::cout << "End of Video Stream with pending " << framesInQueue << " frames in display queue." << std::endl;
            }
        }

        framesInQueue = m_pVideoFrameBuffer->DequeueDecodedPicture(pFrame);
        if (false) {
            std::cout << "Number of frames : " << framesInQueue << std::endl;
        }
    }

    if (framesInQueue) {
        m_videoFrameNum++;

        if (m_videoFrameNum == 1) {
            DumpVideoFormat(m_pDecoder->GetVideoFormatInfo(), true);
        }

        if (m_frameToFile) {
            OutputFrameToFile(pFrame);
        }
    }

    *endOfStream = ((nVideoBytes == 0) || m_videoStreamHasEnded);

    if ((framesInQueue == 0) && m_videoStreamHasEnded) {
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

        return m_pVideoFrameBuffer->ReleaseDisplayedPicture(&decodedFramesReleasePtr, 1);
    }

    return -1;
}

VkResult VulkanVideoProcessor::CreateParser(FFmpegDemuxer* pFFmpegDemuxer,
    const char* filename,
    VkVideoCodecOperationFlagBitsKHR vkCodecType)
{
    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION };
    static const VkExtensionProperties h265StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION };

    const VkExtensionProperties* pStdExtensionVersion = NULL;
    if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
        pStdExtensionVersion = &h264StdExtensionVersion;
    } else if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
        pStdExtensionVersion = &h265StdExtensionVersion;
    } else {
        assert(!"Unsupported Codec Type");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    m_pParser = vulkanCreateVideoParser(m_pDecoder, m_pVideoFrameBuffer, vkCodecType, pStdExtensionVersion, 1, 1, 0);
    if (m_pParser) {
        return VK_SUCCESS;
    } else {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VkResult VulkanVideoProcessor::ParseVideoStreamData(const uint8_t* pData, int size,
                                                    int32_t *pnVideoBytes, bool doPartialParsing,
                                                    uint32_t flags, int64_t timestamp) {
    if (!m_pParser) {
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

    return m_pParser->ParseVideoData(&packet, pnVideoBytes, doPartialParsing);
}
