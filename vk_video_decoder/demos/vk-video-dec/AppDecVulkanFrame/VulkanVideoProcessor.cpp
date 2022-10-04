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

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VulkanVideoProcessor.h"
#include "vulkan_interfaces.h"

inline void CheckInputFile(const char* szInFilePath)
{
    std::ifstream fpIn(szInFilePath, std::ios::in | std::ios::binary);
    if (fpIn.fail()) {
        std::ostringstream err;
        err << "Unable to open input file: " << szInFilePath << std::endl;
        throw std::invalid_argument(err.str());
    }
}

int32_t VulkanVideoProcessor::Init(const VulkanDecodeContext* vulkanDecodeContext, vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo, const char* filePath)
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

    m_pDecoder = new NvVkDecoder(vulkanDecodeContext, m_pVideoFrameBuffer);
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

    const char* pCodec = nvVideoProfile::CodecToName(videoFormat->codec);
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

int32_t VulkanVideoProcessor::GetNextFrames(DecodedFrame* pFrame, bool* endOfStream)
{
    int32_t nVideoBytes = 0, framesInQueue = 0;

    // The below call to DequeueDecodedPicture allows returning the next frame without parsing of the stream.
    // Parsing is only done when there are no more frames in the queue.
    framesInQueue = m_pVideoFrameBuffer->DequeueDecodedPicture(pFrame);

    // Loop until a frame (or more) is parsed and added to the queue.
    while ((framesInQueue == 0) && !m_videoStreamHasEnded) {
        if (!m_videoStreamHasEnded) {
            m_pFFmpegDemuxer->Demux(&m_pBitStreamVideo, &nVideoBytes);
            VkResult parserStatus = VK_ERROR_DEVICE_LOST;
            parserStatus = ParseVideoStreamData(m_pBitStreamVideo, nVideoBytes, &nVideoBytes);
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
    if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        pStdExtensionVersion = &h264StdExtensionVersion;
    } else if (vkCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
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

VkResult VulkanVideoProcessor::ParseVideoStreamData(const uint8_t* pData, int size, int32_t *pnVideoBytes, uint32_t flags, int64_t timestamp) {
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

    return m_pParser->ParseVideoData(&packet, pnVideoBytes);
}
