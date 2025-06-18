/*
 * Copyright 2024 NVIDIA Corporation.
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

#include <iostream>

#include "VkCodecUtils/DecoderConfig.h"
#include "vulkan_video_decoder.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkDecoderUtils/VideoStreamDemuxer.h"

static void DumpDecoderStreamInfo(VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder)
{
    const VkVideoProfileInfoKHR videoProfileInfo = vulkanVideoDecoder->GetVkProfile();

    const VkExtent3D extent = vulkanVideoDecoder->GetVideoExtent();

    std::cout << "Test Video Input Information" << std::endl
               << "\tCodec        : " << VkVideoCoreProfile::CodecToName(videoProfileInfo.videoCodecOperation) << std::endl
               << "\tCoded size   : [" << extent.width << ", " << extent.height << "]" << std::endl
               << "\tChroma Subsampling:";

    VkVideoCoreProfile::DumpFormatProfiles(&videoProfileInfo);
    std::cout << std::endl;
}

static size_t init(std::vector<VulkanDecodedFrame>& frameDataQueue, uint32_t& curFrameDataQueueIndex,
                   const uint32_t decoderQueueSize)
{
    curFrameDataQueueIndex = 0;
    frameDataQueue.resize(decoderQueueSize);
    return frameDataQueue.size();
}

static bool GetNextFrame(VkSharedBaseObj<VulkanVideoDecoder>& vulkanVideoDecoder,
                         std::vector<VulkanDecodedFrame>& frameDataQueue,
                         uint32_t& curFrameDataQueueIndex)
{
    bool continueLoop = true;

    VulkanDecodedFrame& data = frameDataQueue[curFrameDataQueueIndex];
    VulkanDecodedFrame* pLastDecodedFrame = nullptr;

    if (vulkanVideoDecoder->GetWidth() > 0) {

        pLastDecodedFrame = &data;

        vulkanVideoDecoder->ReleaseFrame(pLastDecodedFrame);

        pLastDecodedFrame->Reset();

        bool endOfStream = false;
        int32_t numVideoFrames = 0;

        numVideoFrames = vulkanVideoDecoder->GetNextFrame(pLastDecodedFrame, &endOfStream);
        if (endOfStream && (numVideoFrames < 0)) {
            continueLoop = false;
        }
    }

    // wait for the last submission since we reuse frame data
    const bool dumpDebug = true;
    if (dumpDebug && pLastDecodedFrame) {

        VkSharedBaseObj<VkImageResourceView> imageResourceView;
        pLastDecodedFrame->imageViews[VulkanDecodedFrame::IMAGE_VIEW_TYPE_OPTIMAL_DISPLAY].GetImageResourceView(imageResourceView);

        std::cout << "picIdx: " << pLastDecodedFrame->pictureIndex
                  << "\tdisplayWidth: " << pLastDecodedFrame->displayWidth
                  << "\tdisplayHeight: " << pLastDecodedFrame->displayHeight
                  << "\tdisplayOrder: " << pLastDecodedFrame->displayOrder
                  << "\tdecodeOrder: " << pLastDecodedFrame->decodeOrder
                  << "\ttimestamp " << pLastDecodedFrame->timestamp
                  << "\tdstImageView " << (imageResourceView ? imageResourceView->GetImageResource()->GetImage() : VkImage())
                  << std::endl;
    }

    curFrameDataQueueIndex = (curFrameDataQueueIndex + 1) % frameDataQueue.size();

    return continueLoop;
}

static void deinit(std::vector<VulkanDecodedFrame>& frameDataQueue,
                   uint32_t& curFrameDataQueueIndex)
{
    frameDataQueue.clear();
    curFrameDataQueueIndex = 0;
}

int main(int argc, const char** argv)
{
    std::cout << "Enter decoder test" << std::endl;

    DecoderConfig decoderConfig(argv[0]);
    bool configResult = decoderConfig.ParseArgs(argc, argv);
    if (!configResult && (decoderConfig.help == true)) {
        return 0;
    } else if (!configResult) {
        return -1;
    }

    switch (decoderConfig.forceParserType)
    {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
            break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
            break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
            break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
            break;
        default:
            std::cout << "Simple decoder does not support demuxing "
                      << "and the decoder type must be set with --codec <codec type>"
                      << std::endl;
            return -1;
    }

    VkSharedBaseObj<VideoStreamDemuxer> videoStreamDemuxer;
    VkResult result = VideoStreamDemuxer::Create(decoderConfig.videoFileName.c_str(),
                                                 decoderConfig.forceParserType,
                                                 decoderConfig.enableStreamDemuxing,
                                                 decoderConfig.initialWidth,
                                                 decoderConfig.initialHeight,
                                                 decoderConfig.initialBitdepth,
                                                 videoStreamDemuxer);

    if (result != VK_SUCCESS) {
        assert(!"Can't initialize the VideoStreamDemuxer!");
        return result;
    }

    VkSharedBaseObj<VkVideoFrameOutput> frameToFile;
    if (!decoderConfig.outputFileName.empty()) {
        const char* crcOutputFile = decoderConfig.outputcrcPerFrame ? decoderConfig.crcOutputFileName.c_str() : nullptr;
        result = VkVideoFrameOutput::Create(decoderConfig.outputFileName.c_str(),
                                          decoderConfig.outputy4m,
                                          decoderConfig.outputcrcPerFrame,
                                          crcOutputFile,
                                          decoderConfig.crcInitValue,
                                          frameToFile);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "Error creating output file %s\n", decoderConfig.outputFileName.c_str());
            return -1;
        }
    }

    VkSharedBaseObj<VulkanVideoDecoder> vulkanVideoDecoder;
    result = CreateVulkanVideoDecoder(VK_NULL_HANDLE,
                                      VK_NULL_HANDLE,
                                      VK_NULL_HANDLE,
                                      videoStreamDemuxer,
                                      frameToFile,
                                      nullptr,
                                      argc, argv,
                                      vulkanVideoDecoder);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Error creating video decoder\n");
        return -1;
    }

    DumpDecoderStreamInfo(vulkanVideoDecoder);

    std::vector<VulkanDecodedFrame> frameDataQueue;
    uint32_t                        curFrameDataQueueIndex = 0;

    frameDataQueue.resize(decoderConfig.decoderQueueSize);

    init(frameDataQueue, curFrameDataQueueIndex, decoderConfig.decoderQueueSize);

    bool continueLoop = true;
    do {
        continueLoop = GetNextFrame(vulkanVideoDecoder, frameDataQueue, curFrameDataQueueIndex);
    } while (continueLoop);

    deinit(frameDataQueue, curFrameDataQueueIndex);

    std::cout << "Exit decoder test" << std::endl;
}


