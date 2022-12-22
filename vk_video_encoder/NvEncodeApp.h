/*
 * Copyright 2022 NVIDIA Corporation.
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

#define VK_ENABLE_BETA_EXTENSIONS 1

#include "vulkan/vulkan.hpp"

#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/NvVideoSession.h"
#include "VkCodecUtils/NvPictureBuffer.h"

#include "nvh/fileoperations.hpp"
#include "nvh/inputparser.h"
#include "mio/mio.hpp"


#define NON_VCL_BITSTREAM_OFFSET 4096

static const uint32_t H264MbSizeAlignment = 16;

template<typename sizeType>
sizeType AlignSize(sizeType size, sizeType alignment) {
    assert((alignment & (alignment - 1)) == 0);
    return (size + alignment -1) & ~(alignment -1);
}

struct EncodeConfig {
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t alignedWidth;
    uint32_t alignedHeight;
    uint32_t lumaPlaneSize;
    uint32_t chromaPlaneSize;
    uint32_t fullImageSize;
    uint32_t startFrame;
    uint32_t numFrames;
    uint32_t codecBlockAlignment; // 16 - H264
    uint32_t qp;
    char inFileName[256];
    char outFileName[256];
    uint32_t chromaFormatIDC;
    VkFormat inputVkFormat;
    uint32_t bytepp; // 1 bytepp = 8 bpp
    uint32_t bpp;
    FILE *inputVid; // YUV file
    mio::basic_mmap<mio::access_mode::read, uint8_t> inputVideoMmap;
    FILE *outputVid; // compressed H264 file
    uint32_t logBatchEncoding : 1;
};

class IntraFrameInfo {
public:
    IntraFrameInfo(uint32_t frameCount, uint32_t width, uint32_t height, StdVideoH264SequenceParameterSet sps, StdVideoH264PictureParameterSet pps, bool isIdr);
    inline VkVideoEncodeH264VclFrameInfoEXT* getEncodeH264FrameInfo()
    {
        return &m_encodeH264FrameInfo;
    };
private:
    StdVideoEncodeH264SliceHeaderFlags m_sliceHeaderFlags = {};
    StdVideoEncodeH264SliceHeader m_sliceHeader = {};
    VkVideoEncodeH264NaluSliceInfoEXT m_sliceInfo = {};
    StdVideoEncodeH264PictureInfoFlags m_pictureInfoFlags = {};
    StdVideoEncodeH264PictureInfo m_stdPictureInfo = {};
    VkVideoEncodeH264VclFrameInfoEXT m_encodeH264FrameInfo = {};
};

class VideoSessionParametersInfo {
public:
    VideoSessionParametersInfo(VkVideoSessionKHR videoSession, StdVideoH264SequenceParameterSet* sps, StdVideoH264PictureParameterSet* pps);
    inline VkVideoSessionParametersCreateInfoKHR* getVideoSessionParametersInfo()
    {
        return &m_encodeSessionParametersCreateInfo;
    };
private:
    VkVideoSessionKHR m_videoSession;
    VkVideoEncodeH264SessionParametersAddInfoEXT m_encodeH264SessionParametersAddInfo;
    VkVideoEncodeH264SessionParametersCreateInfoEXT m_encodeH264SessionParametersCreateInfo;
    VkVideoSessionParametersCreateInfoKHR m_encodeSessionParametersCreateInfo;
};

class EncodeInfo {
public:
    inline VkVideoEncodeInfoKHR* getVideoEncodeInfo()
    {
        return &m_encodeInfo;
    };
protected:
    VkVideoEncodeInfoKHR m_encodeInfo;
};

class EncodeInfoNonVcl : public EncodeInfo {
public:
    EncodeInfoNonVcl(StdVideoH264SequenceParameterSet* sps, StdVideoH264PictureParameterSet* pps, VkBuffer* dstBitstreamBuffer)
        : m_emitParameters{}
    {
        m_emitParameters.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_EMIT_PICTURE_PARAMETERS_INFO_EXT;
        m_emitParameters.pNext = NULL;
        m_emitParameters.spsId = sps->seq_parameter_set_id;
        m_emitParameters.emitSpsEnable = VK_TRUE;
        m_emitParameters.ppsIdEntryCount = 1;
        m_emitParameters.ppsIdEntries = &pps->pic_parameter_set_id;

        memset(&m_encodeInfo, 0, sizeof(m_encodeInfo));
        m_encodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
        m_encodeInfo.pNext = &m_emitParameters;
        m_encodeInfo.dstBitstreamBuffer = *dstBitstreamBuffer;
    }
private:
    VkVideoEncodeH264EmitPictureParametersInfoEXT m_emitParameters;
};

class EncodeInfoVcl : public EncodeInfo {
public:
    EncodeInfoVcl(VkBuffer* dstBitstreamBuffer, VkDeviceSize dstBitstreamBufferOffset, VkVideoEncodeH264VclFrameInfoEXT* encodeH264FrameInfo,
                  VkVideoPictureResourceInfoKHR* inputPicResource, VkVideoPictureResourceInfoKHR* dpbPicResource)
        : m_referenceSlot{}
    {
        m_referenceSlot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        m_referenceSlot.pNext = NULL;
        m_referenceSlot.slotIndex = 0;
        m_referenceSlot.pPictureResource = dpbPicResource;

        memset(&m_encodeInfo, 0, sizeof(m_encodeInfo));
        m_encodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
        m_encodeInfo.pNext = encodeH264FrameInfo;
        m_encodeInfo.qualityLevel = 0;
        m_encodeInfo.dstBitstreamBuffer = *dstBitstreamBuffer;
        m_encodeInfo.dstBitstreamBufferOffset = dstBitstreamBufferOffset;
        m_encodeInfo.srcPictureResource = *inputPicResource;
        m_encodeInfo.pSetupReferenceSlot = &m_referenceSlot;
    }
private:
    VkVideoReferenceSlotInfoKHR m_referenceSlot;
};

class NvVideoSessionParameters {
public:
    StdVideoH264SequenceParameterSet m_sequenceParameterSet;
    StdVideoH264PictureParameterSet m_pictureParameterSet;
    VkVideoSessionParametersKHR m_encodeSessionParameters;
};

class EncodeApp {
public:
    EncodeApp()
        : m_ctx()
        , m_cmdPoolVideoEncode()
        , m_videoProfile()
        , m_pVideoSession(NULL)
        , m_imageFormat()
        , m_maxCodedExtent()
        , m_inputNumFrames(0)
        , m_dpbNumFrames(0)
        , m_maxReferencePicturesSlotsCount(0)
        , m_devAlloc()
        , m_resAlloc()
        , m_pictureBuffer()
    {
    };
    int32_t initEncoder(EncodeConfig* encodeConfig);
    int32_t loadFrame(EncodeConfig* encodeConfig, uint32_t frameCount, uint32_t currentFrameBufferIdx);
    int32_t encodeFrame(EncodeConfig* encodeConfig, uint32_t frameCount, bool nonVcl, uint32_t currentFrameBufferIdx);
    int32_t assembleBitstreamData(EncodeConfig* encodeConfig, bool nonVcl, uint32_t currentFrameBufferIdx);
    int32_t batchSubmit(uint32_t firstFrameBufferIdx, uint32_t framesInBatch);
    int32_t deinitEncoder();

    int32_t initRateControl(VkCommandBuffer cmdBuf, uint32_t qp);
    int32_t selectNvidiaGPU(std::vector<uint32_t> compatibleDevices, nvvk::ContextCreateInfo ctxInfo, uint32_t deviceID);
    VkResult getVideoFormats(VkPhysicalDevice physicalDevice, VkVideoCoreProfile* pVideoProfile, VkImageUsageFlags imageUsage, uint32_t& formatCount, VkFormat* formats);
    VkResult getVideoCapabilities(VkPhysicalDevice physicalDevice, VkVideoCoreProfile* pVideoProfile, VkVideoCapabilitiesKHR* pVideoEncodeCapabilities);
    VkVideoComponentBitDepthFlagBitsKHR getComponentBitDepthFlagBits(uint32_t bpp);
    VkVideoChromaSubsamplingFlagBitsKHR getChromaSubsamplingFlagBits(uint32_t chromaFormatIDC);
    StdVideoH264SequenceParameterSet getStdVideoH264SequenceParameterSet (uint32_t width, uint32_t height, StdVideoH264SequenceParameterSetVui*	stdVideoH264SequenceParameterSetVui);
    StdVideoH264PictureParameterSet getStdVideoH264PictureParameterSet ();

    void convertYUVpitchtoNV12(const uint8_t *yuv_luma, const uint8_t *yuvCb, const uint8_t *yuv_cr, uint8_t *nv12Luma,
                               uint8_t *nv12_chroma, int32_t width, int32_t height, int32_t srcStride, int32_t dstStride);
    const uint8_t* setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset);
    int32_t loadCurrentFrame(uint8_t *nv12Input[2], mio::basic_mmap<mio::access_mode::read, uint8_t>& inputVideoMmap,
                             uint32_t frameIndex, uint32_t width, uint32_t height,
                             uint32_t srcStride, uint32_t dstStride, VkFormat inputVkFormat);


private:
    nvvk::Context m_ctx;
    nvvk::CommandPool m_cmdPoolVideoEncode;
    VkVideoCoreProfile m_videoProfile;
    NvVideoSession* m_pVideoSession;
    NvVideoSessionParameters m_videoSessionParameters;
    VkFormat m_imageFormat;
    VkExtent2D m_maxCodedExtent;
    uint32_t m_inputNumFrames;
    uint32_t m_dpbNumFrames;
    uint32_t m_maxReferencePicturesSlotsCount;
    nvvk::DedicatedMemoryAllocator m_devAlloc;
    nvvk::ResourceAllocatorDedicated m_resAlloc;
    NvPictureBuffer m_pictureBuffer;
    nvvk::Context::Queue m_queue;
};
