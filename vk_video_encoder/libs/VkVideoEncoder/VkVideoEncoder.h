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

#include "VkVideoEncoderDef.h"

#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/VulkanVideoSession.h"
#include "VkEncoderPictureBuffer.h"
#include "VkEncoderDpbH264.h"

#include "mio/mio.hpp"

struct EncodePerFrameConstConfigh264 {

    EncodePerFrameConstConfigh264() : flags(0) {

    }
    uint32_t flags;
};

struct EncodePerFrameConstConfig {

    EncodePerFrameConstConfig(StdVideoH26XPictureType pictureType)
        : m_pictureType(pictureType)
        , h264() {

    }

    StdVideoH26XPictureType m_pictureType;

    union {
        EncodePerFrameConstConfigh264 h264;
    };
};


struct EncodeConfigh264 {

    EncodeConfigh264()
        : disable_deblocking_filter_idc(STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_ENABLED)
    {}

    StdVideoH264DisableDeblockingFilterIdc disable_deblocking_filter_idc;
};

struct EncodeConfig {
    std::string name;
    int32_t  deviceId;
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
    uint32_t maxNumRefFrames;
    uint32_t gopFrameCount;       // Specifies the number of pictures in one GOP.
    uint32_t idrPeriod;           // Specifies the IDR interval. If not set, this is made equal to gopLength
    uint32_t numSlicesPerPicture; // sliceModeData specifies number of slices in the picture.
    VkVideoEncodeH264RateControlInfoEXT        m_rcInfoH264;
    VkVideoEncodeH264RateControlLayerInfoEXT   m_rcLayerInfoH264;
    VkVideoEncodeRateControlInfoKHR            m_rcInfo;
    VkVideoEncodeRateControlLayerInfoKHR       m_rcLayerInfo;
    const EncodePerFrameConstConfig*           m_perFrameConfig;
    uint32_t                                   m_perFrameConfigSize;
    const EncodePerFrameConstConfig            m_firstFrameConfig;
    const EncodePerFrameConstConfig            m_lastFrameConfig;
    char inFileName[256];
    char outFileName[256];
    uint32_t chromaFormatIDC;
    VkFormat inputVkFormat;
    uint32_t bpp;
    FILE *inputVid; // YUV file
    mio::basic_mmap<mio::access_mode::read, uint8_t> inputVideoMmap;
    FILE *outputVid; // compressed H264 file
    uint32_t logBatchEncoding : 1;
    uint32_t validate : 1;
    uint32_t validateVerbose : 1;
    uint32_t verbose : 1;
    EncodeConfigh264 h264;

    enum { DEFAULT_GOP_LENGTH = 30 };
    enum { DEFAULT_NUM_SLICES_PER_PICTURE = 4 };
    enum { DEFAULT_MAX_NUM_REF_FRAMES = 4 };

    EncodeConfig()
    : name()
    , deviceId(-1)
    , codec(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT)
    , width(0)
    , height(0)
    , alignedWidth(0)
    , alignedHeight(0)
    , lumaPlaneSize(0)
    , chromaPlaneSize(0)
    , fullImageSize(0)
    , startFrame(0)
    , numFrames(0)
    , codecBlockAlignment(16)
    , qp(0)
    , maxNumRefFrames(DEFAULT_MAX_NUM_REF_FRAMES)
    , gopFrameCount(DEFAULT_GOP_LENGTH)
    , idrPeriod(gopFrameCount)
    , numSlicesPerPicture(DEFAULT_NUM_SLICES_PER_PICTURE)
    , m_rcInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_EXT }
    , m_rcLayerInfoH264{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT }
    , m_rcInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR, &m_rcInfoH264 }
    , m_rcLayerInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR, &m_rcLayerInfoH264 }
    , m_perFrameConfig()
    , m_perFrameConfigSize(0)
    , m_firstFrameConfig(STD_VIDEO_H26X_PICTURE_TYPE_INTRA_REFRESH)
    , m_lastFrameConfig(STD_VIDEO_H26X_PICTURE_TYPE_I)
    , inFileName{}
    , outFileName{}
    , chromaFormatIDC(STD_VIDEO_H264_CHROMA_FORMAT_IDC_420)
    , inputVkFormat(VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
    , bpp(8)
    , inputVid()
    , inputVideoMmap()
    , outputVid() // compressed H264 file
    , logBatchEncoding(false)
    , validate(false)
    , validateVerbose(false)
    , verbose(false)
    , h264()
    {
        static const EncodePerFrameConstConfig perFrameConfig[] = {
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_I),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_P),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_P),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_P),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_B),
            EncodePerFrameConstConfig(STD_VIDEO_H26X_PICTURE_TYPE_I),
        };

        m_perFrameConfig = perFrameConfig;
        m_perFrameConfigSize = sizeof(perFrameConfig) / sizeof(perFrameConfig[0]);
    }
};

class IntraFrameInfo {
public:
    inline VkVideoEncodeH264PictureInfoEXT* getEncodeH264FrameInfo()
    {
        return &m_encodeH264FrameInfo;
    };
private:
    StdVideoEncodeH264SliceHeaderFlags m_sliceHeaderFlags = {};
    StdVideoEncodeH264SliceHeader m_sliceHeader = {};
    VkVideoEncodeH264NaluSliceInfoEXT m_sliceInfo = {};
    StdVideoEncodeH264PictureInfoFlags m_pictureInfoFlags = {};
    StdVideoEncodeH264PictureInfo m_stdPictureInfo = {};
    VkVideoEncodeH264PictureInfoEXT m_encodeH264FrameInfo = {};
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

class EncodeInfoVcl : public EncodeInfo {
public:
    EncodeInfoVcl(VkBuffer dstBitstreamBuffer,
                  VkDeviceSize dstBitstreamBufferOffset,
                  VkVideoEncodeH264PictureInfoEXT* encodeH264FrameInfo,
                  VkVideoPictureResourceInfoKHR* inputPicResource,
                  VkVideoPictureResourceInfoKHR* dpbPicResource)
        : m_referenceSlot{}
    {
        m_referenceSlot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        m_referenceSlot.pNext = NULL;
        m_referenceSlot.slotIndex = 0;
        m_referenceSlot.pPictureResource = dpbPicResource;

        memset(&m_encodeInfo, 0, sizeof(m_encodeInfo));
        m_encodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
        m_encodeInfo.pNext = encodeH264FrameInfo;
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

struct EncoderH264State {

    EncoderH264State()
        : m_spsInfo()
        , m_ppsInfo()
        , m_vuiInfo()
        , m_hrdParameters()
        , m_rcInfoH264()
        , m_rcLayerInfoH264()
    {}

public:
    StdVideoH264SequenceParameterSet         m_spsInfo;
    StdVideoH264PictureParameterSet          m_ppsInfo;
    StdVideoH264SequenceParameterSetVui      m_vuiInfo;
    StdVideoH264HrdParameters                m_hrdParameters;
    VkVideoEncodeH264RateControlInfoEXT      m_rcInfoH264;
    VkVideoEncodeH264RateControlLayerInfoEXT m_rcLayerInfoH264;
};

struct EncPicParamsH264
{
    EncPicParamsH264()
        : displayPOCSyntax()
        , numSlices()
    {}
    uint32_t displayPOCSyntax;
    uint32_t numSlices; // The number of slices, as computed from sliceMode and sliceModeData.
};

/**
 * QP value for frames
 */
struct ConstQpSettings
{
    ConstQpSettings()
        : qpInterP(0)
        , qpInterB(0)
        , qpIntra(0)
    { }

    uint32_t qpInterP;
    uint32_t qpInterB;
    uint32_t qpIntra;
};

struct EncPicParams
{
    EncPicParams()
        : pictureType(STD_VIDEO_H26X_PICTURE_TYPE_IDR)
        , inputTimeStamp(0)
        , nonVclDataSize(0)
        , bitstreamHeaderOffset(0)
        , bitstreamHeaderBuffer{}
        , constQp()
        , qualityLevel()
        , h264()
        , lastFrame(false)
    {}

    StdVideoH26XPictureType pictureType;
    uint64_t                inputTimeStamp;
    size_t                  nonVclDataSize;
    uint32_t                bitstreamHeaderOffset;
    uint8_t                 bitstreamHeaderBuffer[256]; // Valid if nonVclDataSize > 0
    VkVideoPictureResourceInfoKHR refPicList[18]; /* List of reference pictures 16 + 2 for current */
    ConstQpSettings         constQp;
    uint32_t                qualityLevel;
    EncPicParamsH264        h264;
    uint32_t                lastFrame : 1;
};

class EncodeApp {
public:
    EncodeApp(const VulkanDeviceContext* vkDevCtx)
        : m_vkDevCtx(vkDevCtx)
        , m_videoProfile()
        , m_videoSession()
        , m_imageDpbFormat()
        , m_imageInFormat()
        , m_maxCodedExtent()
        , m_inputNumFrames(0)
        , m_dpbNumFrames(0)
        , m_maxReferencePicturesSlotsCount(0)
        , m_videoFrameBuffer()
        , m_rcInfo()
        , m_rcLayerInfo()
        , m_dpb264()
        , m_maxDpbSlots(0)
        , m_picIdxToDpb{}
        , m_dpbSlotsMask(0)
        , m_h264()
        , m_frameNumSyntax(0)
        , m_IDRPicId(0)
        , m_videoMaintenance1FeaturesSupported(false)
        , m_sendControlCmd(true)
        , m_sendResetControlCmd(true)
        , m_rateControlTestMode(true)
        , m_useImageArray(false)
        , m_useImageViewArray(false)
        , m_useSeparateOutputImages(false)
        , m_useLinearInput(false)
        , m_resetEncoder(false)
        , m_verbose(false)
    {
    };

    ~EncodeApp() {
        DeinitEncoder();
    }

    int32_t InitEncoder(EncodeConfig* encodeConfig);
    int32_t LoadFrame(EncodeConfig* encodeConfig, uint32_t frameIndexNum, uint32_t currentFrameBufferIdx);
    VkResult EncodeFrame(EncodeConfig* encodeConfig, uint32_t frameCount, bool nonVcl, uint32_t currentFrameBufferIdx);
    VkResult EncodeH264Frame(EncPicParams *pEncPicParams, EncodeConfig* encodeConfig,
                             VkCommandBuffer cmdBuf, uint32_t frameCount, uint32_t currentFrameBufferIdx,
                             VkSharedBaseObj<VkImageResourceView>& srcImageView,
                             VkSharedBaseObj<VkBufferResource>& outBitstream);
    int32_t AssembleBitstreamData(EncodeConfig* encodeConfig, bool nonVcl, uint32_t currentFrameBufferIdx);
    int32_t BatchSubmit(uint32_t firstFrameBufferIdx, uint32_t framesInBatch);
    int32_t DeinitEncoder();

    int32_t InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp);
    VkVideoComponentBitDepthFlagBitsKHR GetComponentBitDepthFlagBits(uint32_t bpp);
    VkVideoChromaSubsamplingFlagBitsKHR GetChromaSubsamplingFlagBits(uint32_t chromaFormatIDC);
    StdVideoH264SequenceParameterSet GetStdVideoH264SequenceParameterSet (uint32_t width, uint32_t height, StdVideoH264SequenceParameterSetVui*	stdVideoH264SequenceParameterSetVui);
    StdVideoH264PictureParameterSet GetStdVideoH264PictureParameterSet();

    void ConvertYCbCrPitchToNv12(const uint8_t *lumaChIn, const uint8_t *cbChIn, const uint8_t *crChIn,
                                 int32_t srcStride,
                                 uint8_t *outImagePtr, VkSubresourceLayout outImageLayouts[3],
                                 int32_t width, int32_t height);
    const uint8_t* setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset);
    int32_t LoadCurrentFrame(uint8_t *outImagePtr, VkSubresourceLayout outImageLayouts[3],
                             mio::basic_mmap<mio::access_mode::read, uint8_t>& inputVideoMmap,
                             uint32_t frameIndex, uint32_t srcWidth, uint32_t srcHeight,
                             uint32_t srcStride, VkFormat inputVkFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM);


private:
    void POCBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                  uint8_t& m_refPicMarkingOpCount);

    void FrameNumBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                       uint8_t& m_refPicMarkingOpCount);

    VkResult SetupRefPicReorderingCommands(const StdVideoEncodeH264SliceHeader *slh,
                                           StdVideoEncodeH264ReferenceListsInfoFlags* pFlags,
                                           StdVideoEncodeH264RefListModEntry* m_ref_pic_list_modification_l0,
                                           uint8_t& m_refList0ModOpCount);

    void ResetPicDpbSlot(uint32_t validSlotsMask);
    int8_t SetPicDpbSlot(uint32_t referencePictureIndex, int8_t dpbSlot);
private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkVideoCoreProfile m_videoProfile;
    VkSharedBaseObj<VulkanVideoSession> m_videoSession;
    NvVideoSessionParameters m_sessionParameters;
    VkFormat m_imageDpbFormat;
    VkFormat m_imageInFormat;
    VkExtent2D m_maxCodedExtent;
    uint32_t m_inputNumFrames;
    uint32_t m_dpbNumFrames;
    uint32_t m_maxReferencePicturesSlotsCount;
    VkEncoderPictureBuffer m_videoFrameBuffer;
    VkVideoEncodeRateControlInfoKHR       m_rcInfo;
    VkVideoEncodeRateControlLayerInfoKHR  m_rcLayerInfo;
    VkEncDpbH264           m_dpb264;
    uint32_t               m_maxDpbSlots;
    int32_t                m_picIdxToDpb[MAX_DPB_SIZE + 1];
    uint32_t               m_dpbSlotsMask;
    EncoderH264State       m_h264;
    uint32_t m_frameNumSyntax;
    uint32_t m_IDRPicId;
    uint32_t m_videoMaintenance1FeaturesSupported : 1;
    uint32_t m_sendControlCmd : 1;
    uint32_t m_sendResetControlCmd : 1;
    uint32_t m_rateControlTestMode : 1;
    uint32_t m_useImageArray : 1;
    uint32_t m_useImageViewArray : 1;
    uint32_t m_useSeparateOutputImages : 1;
    uint32_t m_useLinearInput : 1;
    uint32_t m_resetEncoder : 1;
    uint32_t m_verbose : 1;
};
