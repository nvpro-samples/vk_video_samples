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

#include "NvCodecUtils/FFmpegDemuxer.h"
#include "NvVkDecoder/NvVkDecoder.h"

class VulkanVideoProcessor {
public:
    int32_t Init(const VulkanDecodeContext* vulkanDecodeContext, vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo, const char* filePath);

    VkFormat GetFrameImageFormat(int32_t* pWidth = NULL, int32_t* pHeight = NULL, int32_t* pBitDepth = NULL);

    int32_t GetWidth();
    int32_t GetHeight();
    int32_t GetBitDepth();

    void Deinit();

    VulkanVideoProcessor()
        : m_pFFmpegDemuxer()
        , m_pVideoFrameBuffer()
        , m_pDecoder()
        , m_pParser()
        , m_pBitStreamVideo(NULL)
        , m_videoFrameNum()
        , m_videoStreamHasEnded(false)
    {
    }

    operator bool() const { return m_pDecoder != NULL; }

    ~VulkanVideoProcessor() { Deinit(); }

    static void DumpVideFormat(const VkParserDetectedVideoFormat* videoFormat, bool dumpData);

    int32_t GetNextFrames(DecodedFrame* pFrame, bool* endOfStream);

    int32_t ReleaseDisplayedFrame(DecodedFrame* pDisplayedFrame);

private:
    VkResult CreateParser(FFmpegDemuxer* pFFmpegDemuxer,
        const char* filename,
        VkVideoCodecOperationFlagBitsKHR vkCodecType);

    VkResult ParseVideoStreamData(const uint8_t* pData, int size, uint32_t flags = 0, int64_t timestamp = 0);

private:
    FFmpegDemuxer* m_pFFmpegDemuxer;
    VulkanVideoFrameBuffer* m_pVideoFrameBuffer;
    NvVkDecoder* m_pDecoder;
    IVulkanVideoParser* m_pParser;
    uint8_t* m_pBitStreamVideo;
    uint32_t m_videoFrameNum;
    uint32_t m_videoStreamHasEnded;
};

#endif /* _VULKANVIDEOPROCESSOR_H_ */
