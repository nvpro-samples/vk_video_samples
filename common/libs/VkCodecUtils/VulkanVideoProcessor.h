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

#include "VkDecoderUtils/VideoStreamDemuxer.h"
#include "VkVideoDecoder/VkVideoDecoder.h"
#include "VkCodecUtils/ProgramConfig.h"

class VkFrameVideoToFile {

public:

    VkFrameVideoToFile()
        : m_outputFile(),
          m_pLinearMemory()
        , m_allocationSize() {}

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

    uint8_t* EnsureAllocation(const VulkanDeviceContext* vkDevCtx,
                              VkSharedBaseObj<VkImageResource>& imageResource) {

        if (m_outputFile == nullptr) {
            return nullptr;
        }

        VkDeviceSize imageMemorySize = imageResource->GetImageDeviceMemorySize();

        if ((m_pLinearMemory == nullptr) || (imageMemorySize > m_allocationSize)) {

            if (m_outputFile) {
                fflush(m_outputFile);
            }

            if (m_pLinearMemory != nullptr) {
                delete[] m_pLinearMemory;
                m_pLinearMemory = nullptr;
            }

            // Allocate the memory that will be dumped to file directly.
            m_allocationSize = (size_t)(imageMemorySize);
            m_pLinearMemory = new uint8_t[m_allocationSize];
            if (m_pLinearMemory == nullptr) {
                return nullptr;
            }
            assert(m_pLinearMemory != nullptr);
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

    size_t GetMaxFrameSize() {
        return m_allocationSize;
    }

private:
    FILE*    m_outputFile;
    uint8_t* m_pLinearMemory;
    size_t   m_allocationSize;
};

class VulkanVideoProcessor : public VkVideoRefCountBase {
public:

    static VkSharedBaseObj<VulkanVideoProcessor>& invalidVulkanVideoProcessor;

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoProcessor>& vulkanVideoProcessor = invalidVulkanVideoProcessor);

    int32_t Initialize(const VulkanDeviceContext* vkDevCtx,
                       ProgramConfig& programConfig);

    VkFormat GetFrameImageFormat(int32_t* pWidth = NULL, int32_t* pHeight = NULL, int32_t* pBitDepth = NULL);

    int32_t GetWidth();
    int32_t GetHeight();
    int32_t GetBitDepth();

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

    bool IsValid(void) { return m_vkVideoDecoder; }

    static void DumpVideoFormat(const VkParserDetectedVideoFormat* videoFormat, bool dumpData);

    int32_t ParserProcessNextDataChunk();
    int32_t GetNextFrame(DecodedFrame* pFrame, bool* endOfStream);

    int32_t ReleaseDisplayedFrame(DecodedFrame* pDisplayedFrame);

    size_t OutputFrameToFile(DecodedFrame* pFrame);
    void Restart(void);

private:

    VulkanVideoProcessor(const VulkanDeviceContext* vkDevCtx)
        : m_refCount(0),
          m_vkDevCtx(vkDevCtx),
          m_videoStreamDemuxer()
        , m_vkVideoFrameBuffer()
        , m_vkVideoDecoder()
        , m_vkParser()
        , m_currentBitstreamOffset(0)
        , m_videoFrameNum(0)
        , m_videoStreamsCompleted(false)
        , m_usesStreamDemuxer(false)
        , m_usesFramePreparser(false)
        , m_frameToFile()
        , m_loopCount(1)
        , m_startFrame(0)
        , m_maxFrameCount(-1)
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
    size_t ConvertFrameToNv12(DecodedFrame* pFrame, VkSharedBaseObj<VkImageResource>& imageResource,
                              uint8_t* pOutputBuffer, size_t bufferSize);


    bool StreamCompleted();

private:
    std::atomic<int32_t>       m_refCount;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<VideoStreamDemuxer> m_videoStreamDemuxer;
    VkSharedBaseObj<VulkanVideoFrameBuffer> m_vkVideoFrameBuffer;
    VkSharedBaseObj<VkVideoDecoder> m_vkVideoDecoder;
    VkSharedBaseObj<IVulkanVideoParser> m_vkParser;
    int64_t  m_currentBitstreamOffset;
    uint32_t m_videoFrameNum;
    uint32_t m_videoStreamsCompleted : 1;
    uint32_t m_usesStreamDemuxer : 1;
    uint32_t m_usesFramePreparser : 1;
    VkFrameVideoToFile m_frameToFile;
    int32_t   m_loopCount;
    uint32_t  m_startFrame;
    int32_t   m_maxFrameCount;
};

#endif /* _VULKANVIDEOPROCESSOR_H_ */
