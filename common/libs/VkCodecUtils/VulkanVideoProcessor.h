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

#include "DecoderConfig.h"
#include "VkDecoderUtils/VideoStreamDemuxer.h"
#include "VkVideoDecoder/VkVideoDecoder.h"
#include "VkCodecUtils/VkVideoQueue.h"
#include "VkVideoFrameOutput.h"

#if (_TRANSCODING)
#include "VkVideoEncoder/VkVideoEncoder.h"
#include <vector>
#endif //_TRANSCODING

class VulkanVideoProcessor : public VkVideoQueue<VulkanDecodedFrame> {
public:

    virtual int32_t GetWidth()    const;
    virtual int32_t GetHeight()   const;
    virtual int32_t GetBitDepth() const;
    virtual VkVideoProfileInfoKHR GetVkProfile() const;
    virtual uint32_t GetProfileIdc() const;
    virtual VkFormat GetFrameImageFormat()  const;
    virtual VkExtent3D GetVideoExtent() const;
    virtual int32_t GetNextFrame(VulkanDecodedFrame* pFrame, bool* endOfStream
#if (_TRANSCODING)
        , DecoderConfig* programConfig = nullptr, VkSharedBaseObj<EncoderConfig>* encoderConfig = nullptr
    #endif // _TRANSCODING
);
#if (_TRANSCODING)
    virtual int32_t GetCodedWidth()    const;
    virtual int32_t GetCodedHeight()   const;
    virtual uint32_t GetFrameRate() const;
    virtual uint32_t GetFramesCount() const;
#endif // _TRANSCODING

    virtual int32_t ReleaseFrame(VulkanDecodedFrame* pDisplayedFrame);

    static VkSharedBaseObj<VulkanVideoProcessor>& invalidVulkanVideoProcessor;

    static VkResult Create(const DecoderConfig& settings, const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoProcessor>& vulkanVideoProcessor = invalidVulkanVideoProcessor);

    int32_t Initialize(const VulkanDeviceContext* vkDevCtx,
                       VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer,
                       VkSharedBaseObj<VkVideoFrameOutput>& frameToFile,
                       DecoderConfig& programConfig);

    void Deinit();

    virtual int32_t AddRef()
    {
        return ++m_refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --m_refCount;
        // Destroy the device if ref-count reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    static void DumpVideoFormat(const VkParserDetectedVideoFormat* videoFormat, bool dumpData);

    int32_t ParserProcessNextDataChunk();

    size_t OutputFrameToFile(VulkanDecodedFrame* pFrame);
    uint32_t Restart(int64_t& bitstreamOffset);

private:

    VulkanVideoProcessor(const DecoderConfig& settings, const VulkanDeviceContext* vkDevCtx)
        : m_refCount(0),
          m_vkDevCtx(vkDevCtx),
          m_videoStreamDemuxer()
        , m_vkVideoFrameBuffer()
        , m_vkVideoDecoder()
#if (_TRANSCODING)
        , m_vkVideoEncoder()
#endif // _TRANSCODING
        , m_vkParser()
        , m_frameToFile()
        , m_currentBitstreamOffset(0)
        , m_videoFrameNum(0)
        , m_videoStreamsCompleted(false)
        , m_usesStreamDemuxer(false)
        , m_usesFramePreparser(false)
        , m_loopCount(1)
        , m_startFrame(0)
        , m_maxFrameCount(-1)
        , m_settings(settings)
    {
    }

    virtual ~VulkanVideoProcessor() { Deinit(); }

    VkResult CreateParser(const char* filename,
                          VkVideoCodecOperationFlagBitsKHR vkCodecType,
                          uint32_t defaultMinBufferSize,
                          uint32_t bufferOffsetAlignment,
                          uint32_t bufferSizeAlignment);

    VkResult ParseVideoStreamData(const uint8_t* pData, size_t size,
                                  size_t* pnVideoBytes = nullptr,
                                  bool doPartialParsing = false,
                                  uint32_t flags = 0, int64_t timestamp = 0);

    bool StreamCompleted();

#if (_TRANSCODING)
public:
    VkSharedBaseObj<VkVideoEncoder> getEncoder(int imgLayerIdx = 0) const {
        return m_vkVideoEncoder[imgLayerIdx];
    };
#endif // _TRANSCODING

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<VideoStreamDemuxer> m_videoStreamDemuxer;
    VkSharedBaseObj<VulkanVideoFrameBuffer> m_vkVideoFrameBuffer;
    VkSharedBaseObj<VkVideoDecoder> m_vkVideoDecoder;
#if (_TRANSCODING)
    std::vector<VkSharedBaseObj<VkVideoEncoder>> m_vkVideoEncoder;
#endif // _TRANSCODING
    VkSharedBaseObj<IVulkanVideoParser> m_vkParser;
    VkSharedBaseObj<VkVideoFrameOutput> m_frameToFile;
    int64_t  m_currentBitstreamOffset;
    uint32_t m_videoFrameNum;
    uint32_t m_videoStreamsCompleted : 1;
    uint32_t m_usesStreamDemuxer : 1;
    uint32_t m_usesFramePreparser : 1;
    int32_t   m_loopCount;
    uint32_t  m_startFrame;
    int32_t   m_maxFrameCount;
    const DecoderConfig& m_settings;
};

#endif /* _VULKANVIDEOPROCESSOR_H_ */
