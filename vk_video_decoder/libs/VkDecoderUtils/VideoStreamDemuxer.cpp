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

#include <sstream>
#include <fstream>
#include "VkDecoderUtils/VideoStreamDemuxer.h"

bool VideoStreamDemuxer::CheckFile(const char* szInFilePath)
{
    std::ifstream fpIn(szInFilePath, std::ios::in | std::ios::binary);
    if (fpIn.fail()) {
        std::ostringstream err;
        err << "Unable to open input file: " << szInFilePath << std::endl;
#ifdef __cpp_exceptions
        throw std::invalid_argument(err.str());
#endif
    }
    return true;
}

VkResult VideoStreamDemuxer::Create(const char *pFilePath,
                                    VkVideoCodecOperationFlagBitsKHR codecType,
                                    bool requiresStreamDemuxing,
                                    int32_t defaultWidth,
                                    int32_t defaultHeight,
                                    int32_t defaultBitDepth,
                                    VkSharedBaseObj<VideoStreamDemuxer>& videoStreamDemuxer)
{
    VideoStreamDemuxer::CheckFile(pFilePath);

#ifdef FFMPEG_DEMUXER_SUPPORT
    if (requiresStreamDemuxing || (codecType == VK_VIDEO_CODEC_OPERATION_NONE_KHR)) {
        return FFmpegDemuxerCreate(pFilePath,
                                   codecType,
                                   requiresStreamDemuxing,
                                   defaultWidth,
                                   defaultHeight,
                                   defaultBitDepth,
                                   videoStreamDemuxer);
    }  else
#endif // FFMPEG_DEMUXER_SUPPORT
    {
        assert(codecType != VK_VIDEO_CODEC_OPERATION_NONE_KHR);
        assert(defaultWidth > 0);
        assert(defaultHeight > 0);
        assert((defaultBitDepth == 8) || (defaultBitDepth == 10) || (defaultBitDepth == 12));
        return ElementaryStreamCreate(pFilePath,
                                      codecType,
                                      defaultWidth,
                                      defaultHeight,
                                      defaultBitDepth,
                                      videoStreamDemuxer);
    }
}



