/*
* Copyright 2021 NVIDIA Corporation.
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

#ifndef _VULKANVIDEOPARSER_H_
#define _VULKANVIDEOPARSER_H_

#include "VkParserVideoRefCountBase.h"
#include "VulkanVideoParserParams.h"

struct VkPictureParameters;

class IVulkanVideoDecoderHandler : public VkParserVideoRefCountBase {
public:
    virtual int32_t StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat) = 0;

    virtual bool UpdatePictureParameters(VkPictureParameters* pPictureParameters,
                                         VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
                                         uint64_t updateSequenceCount) = 0;

    virtual int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo) = 0;
    virtual bool IsDstDpbDistinctImages() = 0;

    virtual ~IVulkanVideoDecoderHandler() { }
};

struct VulkanVideoDisplayPictureInfo;
class vkPicBuffBase;

class IVulkanVideoFrameBufferParserCb : public VkParserVideoRefCountBase {
public:
    virtual int32_t QueueDecodedPictureForDisplay(int8_t picId, VulkanVideoDisplayPictureInfo* pDispInfo) = 0;

    virtual ~IVulkanVideoFrameBufferParserCb() { }

    virtual vkPicBuffBase* ReservePictureBuffer() = 0;
};

struct VkParserSourceDataPacket;
class IVulkanVideoParser : public VkParserVideoRefCountBase {
public:
    static IVulkanVideoParser* CreateInstance(IVulkanVideoDecoderHandler* pDecoderHandler,
        IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
        VkVideoCodecOperationFlagBitsKHR codecType,
        uint32_t maxNumDecodeSurfaces,
        uint32_t maxNumDpbSurfaces,
        uint64_t clockRate,
        uint32_t errorThreshold = 0);

    // doPartialParsing 0: parse entire packet, 1: parse until next decode/display event
    virtual VkResult ParseVideoData(VkParserSourceDataPacket* pPacket,
                                    int32_t* pParsedBytes,
                                    bool doPartialParsing = false) = 0;

protected:
    virtual ~IVulkanVideoParser() { }
};

class IVulkanVideoParser;
class IVulkanVideoDecoderHandler;
class IVulkanVideoFrameBufferParserCb;

extern "C" IVulkanVideoParser* VKAPI_CALL vulkanCreateVideoParser(IVulkanVideoDecoderHandler* pDecoderHandler,
    IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
    VkVideoCodecOperationFlagBitsKHR codecType,
    const VkExtensionProperties* pStdExtensionVersion,
    uint32_t maxNumDecodeSurfaces,
    uint32_t maxNumDpbSurfaces,
    uint64_t clockRate);

#endif /* _VULKANVIDEOPARSER_H_ */
