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

#ifndef _VULKANVIDEOPROCESSOR_H_
#define _VULKANVIDEOPROCESSOR_H_

#ifdef USE_GST_PARSERES
#   include "NvCodecUtils/GstDemuxer.h"
#define VULKAN_VIDEO_DEMUXER GstDemuxer
#define VULKAN_VIDEO_CODEC_CONVERTER GStreamer2NvCodecId
#else
#   include "NvCodecUtils/FFmpegDemuxer.h"
#define VULKAN_VIDEO_DEMUXER FFmpegDemuxer
#define VULKAN_VIDEO_CODEC_CONVERTER FFmpeg2NvCodecId
#endif

#include "NvVkDecoder/NvVkDecoder.h"

class VkFrameVideoToFile {

public:

    VkFrameVideoToFile()
        : m_outputFile(),
          m_pLinearMemory()
        , m_allocationSize()
        , m_width()
        , m_height()
        , m_format(VK_FORMAT_UNDEFINED) {}

    ~VkFrameVideoToFile()
    {
        if (m_pLinearMemory) {
            delete[] m_pLinearMemory;
            m_pLinearMemory = nullptr;
        }

        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }
    }

    uint8_t* EnsureAllocation(VkDevice device, VkImage outputImage,
                              VkFormat format, int32_t width, int32_t height) {

        if (m_outputFile == nullptr) {
            return nullptr;
        }

        if ((m_pLinearMemory == nullptr) || (m_format != format) ||
                (m_width < width) || (m_height < height)) {
            VkMemoryRequirements memReqs = {};
            vk::GetImageMemoryRequirements(device, outputImage, &memReqs);
            if (m_pLinearMemory != nullptr) {
                delete[] m_pLinearMemory;
                m_pLinearMemory = nullptr;
            }

            fflush(m_outputFile);

            // Allocate the memory that will be dumped to file directly.
            m_allocationSize = (size_t)(memReqs.size);
            m_pLinearMemory = new uint8_t[m_allocationSize];
            if (m_pLinearMemory == nullptr) {
                m_width = 0;
                m_height = 0;
                return nullptr;
            }
            assert(m_pLinearMemory != nullptr);
            m_width = width;
            m_height = height;
        }
        return m_pLinearMemory;
    }

    FILE* AttachFile(const char* fileName) {

        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }

        if (fileName != nullptr) {
            m_outputFile = fopen(fileName, "wb");
            if (m_outputFile) {
                return m_outputFile;
            }
        }

        return nullptr;
    }

    bool IsFileStreamValid() const
    {
        return m_outputFile != nullptr;
    }

    operator bool() const {
        return IsFileStreamValid();
    }

    size_t WriteDataToFile(size_t offset, size_t size)
    {
        return fwrite(m_pLinearMemory + offset, size, 1, m_outputFile);
    }

    int32_t GetMaxWidth() {
        return m_width;
    }

    int32_t GetMaxHeight() {
        return m_height;
    }

    size_t GetMaxFrameSize() {
        return m_allocationSize;
    }

private:
    FILE*    m_outputFile;
    uint8_t* m_pLinearMemory;
    size_t   m_allocationSize;
    int32_t  m_width;
    int32_t  m_height;
    VkFormat m_format;
};

class VulkanVideoProcessor {
public:
    int32_t Init(const VulkanDecodeContext* vulkanDecodeContext,
                 vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo,
                 const char* filePath,
                 const char* outputFileName = nullptr,
                 int forceParserType = 0);

    VkFormat GetFrameImageFormat(int32_t* pWidth = NULL, int32_t* pHeight = NULL, int32_t* pBitDepth = NULL);

    int32_t GetWidth();
    int32_t GetHeight();
    int32_t GetBitDepth();

    void Deinit();

    VulkanVideoProcessor()
        : m_pVulkanVideoDemuxer()
        , m_pVideoFrameBuffer()
        , m_pDecoder()
        , m_pParser()
        , m_pBitStreamVideo(NULL)
        , m_videoFrameNum()
        , m_videoStreamHasEnded(false)
        , m_frameToFile()
    {
    }

    bool IsValid(void) {return m_pDecoder != NULL;}

    ~VulkanVideoProcessor() { Deinit(); }

    static void DumpVideoFormat(const VkParserDetectedVideoFormat* videoFormat, bool dumpData);

    int32_t GetNextFrames(DecodedFrame* pFrame, bool* endOfStream);

    int32_t ReleaseDisplayedFrame(DecodedFrame* pDisplayedFrame);

    size_t OutputFrameToFile(DecodedFrame* pFrame);
    void Restart(void);

private:
    VkResult CreateParser(VULKAN_VIDEO_DEMUXER* pVulkanVideoDemuxer,
        const char* filename,
        VkVideoCodecOperationFlagBitsKHR vkCodecType);

    VkResult ParseVideoStreamData(const uint8_t* pData, int size,
                                  int32_t* pnVideoBytes = nullptr,
                                  bool doPartialParsing = false,
                                  uint32_t flags = 0, int64_t timestamp = 0);
    size_t ConvertFrameToNv12(DecodedFrame* pFrame, VkDevice device,
                              VkImage outputImage, VkDeviceMemory imageDeviceMemory, VkFormat format,
                              uint8_t* pOutputBuffer, size_t bufferSize);

private:
    VULKAN_VIDEO_DEMUXER* m_pVulkanVideoDemuxer;
    VulkanVideoFrameBuffer* m_pVideoFrameBuffer;
    NvVkDecoder* m_pDecoder;
    IVulkanVideoParser* m_pParser;
    uint8_t* m_pBitStreamVideo;
    uint32_t m_videoFrameNum;
    uint32_t m_videoStreamHasEnded;
    VkFrameVideoToFile m_frameToFile;
};

#endif /* _VULKANVIDEOPROCESSOR_H_ */
