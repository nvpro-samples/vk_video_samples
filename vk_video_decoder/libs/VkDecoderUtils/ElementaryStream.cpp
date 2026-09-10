/*
 * Copyright 2023 NVIDIA Corporation.  All rights reserved.
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

#include <cassert>
#include <string.h>
#include <fstream>
#include "mio/mio.hpp"
#include "VkDecoderUtils/VideoStreamDemuxer.h"

// Vulkan-Headers >= 1.4.360 added the High 10 and High 4:2:2 enumerators to
// StdVideoH264ProfileIdc. Older SDKs (e.g. the 1.4.341 Windows SDK) stop at
// HIGH_444_PREDICTIVE, so an unguarded reference would restrict this file to a
// header at 1.4.360 or newer: the Linux build fetches such a copy, an installed
// Windows SDK need not be that new. Defining the enumerators when they are
// absent keeps both builds compiling; where the header defines them the shim is
// inert. The values are the H.264 profile_idc numbers themselves, fixed by the
// codec spec.
#ifndef STD_VIDEO_H264_PROFILE_IDC_HIGH_10
#define STD_VIDEO_H264_PROFILE_IDC_HIGH_10  ((StdVideoH264ProfileIdc)110)
#endif
#ifndef STD_VIDEO_H264_PROFILE_IDC_HIGH_422
#define STD_VIDEO_H264_PROFILE_IDC_HIGH_422 ((StdVideoH264ProfileIdc)122)
#endif

#define DKIF_FRAME_CONTAINER_HEADER_SIZE 12
#define DKIF_HEADER_MAGIC *((const uint32_t*)"DKIF")
#define DKIF_FILE_HEADER_SIZE 32

class ElementaryStream : public VideoStreamDemuxer {

public:
    ElementaryStream(const char *pFilePath,
                     VkVideoCodecOperationFlagBitsKHR forceParserType,
                     int32_t defaultWidth,
                     int32_t defaultHeight,
                     int32_t defaultBitDepth,
                     int32_t defaultChroma)
        : VideoStreamDemuxer(),
          m_width(defaultWidth)
        , m_height(defaultHeight)
        , m_bitDepth(defaultBitDepth)
        , m_chroma(defaultChroma)
        , m_videoCodecType(forceParserType)
#ifndef USE_SIMPLE_MALLOC
        , m_inputVideoStreamMmap()
#endif
        , m_pBitstreamData(nullptr)
        , m_bitstreamDataSize(0)
        , m_bytesRead(0) {

#ifdef USE_SIMPLE_MALLOC
        FILE* handle = fopen(pFilePath, "rb");
        if (handle == nullptr) {
            printf("Failed to open video file %s\n", pFilePath);
            m_bitstreamDataSize = 0;
            m_pBitstreamData = nullptr;
            return;
        }

        fseek(handle, 0, SEEK_END);
        size_t size = ftell(handle);
        uint8_t* data = (uint8_t*)malloc(size);

        if (data == nullptr) {
            printf("Failed to allocate memory for video file: %i\n", (uint32_t)size);
            m_bitstreamDataSize = 0;
            return;
        }

        m_bitstreamDataSize = size;
        fseek(handle, 0, SEEK_SET);
        size_t readBytes = fread(data, 1, size, handle);
        if (readBytes != size) {
            free(data);
            fclose(handle);
            return;
        }
        m_pBitstreamData = data;
        fclose(handle);
#else
        std::error_code error;
        m_inputVideoStreamMmap.map(pFilePath, 0, mio::map_entire_file, error);
        if (error) {
            assert(!"Can't map the input stream file!");
        }

        m_bitstreamDataSize = m_inputVideoStreamMmap.mapped_length();

        m_pBitstreamData = m_inputVideoStreamMmap.data();
#endif
        if (m_videoCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            // Assume Duck IVF. DKIF.
            assert(*(const uint32_t*)m_pBitstreamData == DKIF_HEADER_MAGIC);
            const uint32_t firstFrameOffset = (DKIF_FILE_HEADER_SIZE + DKIF_FRAME_CONTAINER_HEADER_SIZE);
            m_pBitstreamData += firstFrameOffset;
            m_bitstreamDataSize -= firstFrameOffset;
        }
    }

    ElementaryStream(const uint8_t *pInput, const size_t,
                     VkVideoCodecOperationFlagBitsKHR codecType)
        : m_width(176)
        , m_height(144)
        , m_bitDepth(8)
        , m_chroma(420)
        , m_videoCodecType(codecType)
#ifndef USE_SIMPLE_MALLOC
        , m_inputVideoStreamMmap()
#endif
        , m_pBitstreamData(pInput)
        , m_bitstreamDataSize(0)
        , m_bytesRead(0)
    {
        if (m_videoCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            // Assume Duck IVF. DKIF.
            assert(*(const uint32_t*)pInput == DKIF_HEADER_MAGIC);
            const uint32_t firstFrameOffset = (DKIF_FILE_HEADER_SIZE + DKIF_FRAME_CONTAINER_HEADER_SIZE);
            m_pBitstreamData += firstFrameOffset;
        }
    }

    int32_t Initialize() { return 0; }

    static VkResult Create(const char *pFilePath,
                           VkVideoCodecOperationFlagBitsKHR codecType,
                           int32_t defaultWidth,
                           int32_t defaultHeight,
                           int32_t defaultBitDepth,
                           int32_t defaultChroma,
                           VkSharedBaseObj<ElementaryStream>& elementaryStream)
    {
        VkSharedBaseObj<ElementaryStream> newElementaryStream(new ElementaryStream(pFilePath, codecType,
                                                                                   defaultWidth,
                                                                                   defaultHeight,
                                                                                   defaultBitDepth,
                                                                                   defaultChroma));

         if ((newElementaryStream) && (newElementaryStream->Initialize() >= 0)) {
             elementaryStream = newElementaryStream;
             return VK_SUCCESS;
         }
         return VK_ERROR_INITIALIZATION_FAILED;
    }

    virtual ~ElementaryStream() {
#ifdef USE_SIMPLE_MALLOC
        if (m_videoCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            const uint32_t firstFrameOffset = (DKIF_FILE_HEADER_SIZE + DKIF_FRAME_CONTAINER_HEADER_SIZE);
            m_pBitstreamData -= firstFrameOffset;
        }

        free((void*)(m_pBitstreamData));
#else
        m_inputVideoStreamMmap.unmap();
#endif
    }

    virtual bool IsStreamDemuxerEnabled() const { return false; }
    virtual bool HasFramePreparser() const { return false; }
    virtual void Rewind() { m_bytesRead = 0; }
    virtual VkVideoCodecOperationFlagBitsKHR GetVideoCodec() const { return m_videoCodecType; }

    virtual VkVideoComponentBitDepthFlagsKHR GetLumaBitDepth() const
    {
        switch (m_bitDepth) {
        case 8:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            break;
        case 10:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
            break;
        case 12:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
            break;
        default:
            assert(!"Unknown Luma Bit Depth!");
        }
        assert(!"Unknown Luma Bit Depth!");
        return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
    }

    virtual VkVideoChromaSubsamplingFlagsKHR GetChromaSubsampling() const
    {
        // A raw elementary stream has no container to read this from, so it comes from
        // --initialChroma. A fixed answer here silently builds a {4:2:0, N-bit} profile
        // for a 4:2:2 or 4:4:4 stream; the driver then rejects that profile with nothing
        // to indicate that the chroma format was the cause.
        switch (m_chroma) {
        case 400: return VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
        case 420: return VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        case 422: return VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
        case 444: return VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
        default:
            assert(!"Unknown chroma subsampling!");
        }
        return VK_VIDEO_CHROMA_SUBSAMPLING_INVALID_KHR;
    }

    virtual VkVideoComponentBitDepthFlagsKHR GetChromaBitDepth() const
    {
        switch (m_bitDepth) {
        case 8:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            break;
        case 10:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
            break;
        case 12:
            return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
            break;
        default:
            assert(!"Unknown Chroma Bit Depth!");
        }
        assert(!"Unknown Chroma Bit Depth!");
        return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
    }
    virtual uint32_t GetProfileIdc() const
    {
        // Derived from the configured bit depth and chroma. A 12-bit or 4:2:2/4:4:4
        // stream is not MAIN in any codec, and announcing MAIN for one builds a profile
        // the driver rejects -- for HEVC in particular, every 12-bit format is bound to
        // Range Extensions (x265 signals 12-bit via general_profile_idc=4), so MAIN
        // cannot match.
        switch (m_videoCodecType) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
            if (m_chroma == 444) {
                return STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
            } else if (m_chroma == 422) {
                return STD_VIDEO_H264_PROFILE_IDC_HIGH_422;
            } else if (m_bitDepth > 8) {
                return STD_VIDEO_H264_PROFILE_IDC_HIGH_10;
            }
            return STD_VIDEO_H264_PROFILE_IDC_MAIN;
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
            if ((m_bitDepth > 10) || (m_chroma == 422) || (m_chroma == 444)) {
                return STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
            } else if (m_bitDepth > 8) {
                return STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
            }
            return STD_VIDEO_H265_PROFILE_IDC_MAIN;
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
            // AV1 Main covers 8/10-bit 4:2:0 and monochrome; 4:2:2 is Professional and
            // 4:4:4 is High.
            if (m_chroma == 422) {
                return STD_VIDEO_AV1_PROFILE_PROFESSIONAL;
            } else if (m_chroma == 444) {
                return STD_VIDEO_AV1_PROFILE_HIGH;
            }
            return STD_VIDEO_AV1_PROFILE_MAIN;
        default:
            assert(0);
        }
        return (uint32_t)(-1);
    }

    virtual int32_t GetWidth() const { return m_width; }
    virtual int32_t GetHeight() const { return m_height; }
    virtual int32_t GetBitDepth() const { return m_bitDepth; }
    virtual int64_t DemuxFrame(const uint8_t**) {
        return -1;
    }
    virtual int64_t ReadBitstreamData(const uint8_t **ppVideo, int64_t offset)
    {
        assert(m_bitstreamDataSize != 0);
        assert(m_pBitstreamData != nullptr);

        // Compute and return the pointer to data at new offset.
        if (m_videoCodecType == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            *ppVideo = (m_pBitstreamData + offset);
            uint32_t dataSize = *(const uint32_t*)(*ppVideo - DKIF_FRAME_CONTAINER_HEADER_SIZE);
            if ((m_bitstreamDataSize - (offset + dataSize)) == 0) {
                return dataSize;
            }

            return dataSize + DKIF_FRAME_CONTAINER_HEADER_SIZE;
        } else {
            *ppVideo = (m_pBitstreamData + offset);
            return m_bitstreamDataSize - offset;
        }
    }

    virtual void DumpStreamParameters() const {
    }

private:
    int32_t    m_width, m_height, m_bitDepth;
    int32_t    m_chroma;   // 400 | 420 | 422 | 444
    VkVideoCodecOperationFlagBitsKHR m_videoCodecType;
#ifndef USE_SIMPLE_MALLOC
    mio::basic_mmap<mio::access_mode::read, uint8_t> m_inputVideoStreamMmap;
#endif
    const uint8_t* m_pBitstreamData;
    VkDeviceSize   m_bitstreamDataSize;
    VkDeviceSize   m_bytesRead;
};

VkResult ElementaryStreamCreate(const char *pFilePath,
                                VkVideoCodecOperationFlagBitsKHR codecType,
                                int32_t defaultWidth,
                                int32_t defaultHeight,
                                int32_t defaultBitDepth,
                                int32_t defaultChroma,
                                VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer)
{
    VkSharedBaseObj<ElementaryStream> elementaryStream;
    VkResult result = ElementaryStream::Create(pFilePath,
                                               codecType,
                                               defaultWidth,
                                               defaultHeight,
                                               defaultBitDepth,
                                               defaultChroma,
                                               elementaryStream);
    if (result == VK_SUCCESS) {
        videoStreamDemuxer = elementaryStream;
    }

    return result;
}
