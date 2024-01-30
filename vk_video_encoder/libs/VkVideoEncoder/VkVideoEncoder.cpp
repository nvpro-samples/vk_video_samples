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

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

#define DECODED_PICTURE_BUFFER_SIZE 16
#define INPUT_FRAME_BUFFER_SIZE 16

#define NON_VCL_BITSTREAM_OFFSET 4096

void EncodeApp::ConvertYCbCrPitchToNv12(const uint8_t *lumaChIn, const uint8_t *cbChIn, const uint8_t *crChIn,
                                        int32_t srcStride,
                                        uint8_t *outImagePtr, VkSubresourceLayout outImageLayouts[3],
                                        int32_t width, int32_t height)
{
    uint8_t *nv12Luma = outImagePtr + outImageLayouts[0].offset;
    for (int32_t y = 0; y < height; y++) {
        memcpy(nv12Luma + (outImageLayouts[0].rowPitch * y), lumaChIn + (srcStride * y), width);
    }

    uint8_t *nv12Chroma = outImagePtr + outImageLayouts[1].offset;
    for (int32_t y = 0; y < (height + 1) / 2; y++) {
        for (int32_t x = 0; x < width; x += 2) {
            nv12Chroma[(y * outImageLayouts[1].rowPitch) + x] = cbChIn[(((srcStride + 1) / 2) * y) + (x >> 1)];
            nv12Chroma[(y * outImageLayouts[1].rowPitch) + (x + 1)] = crChIn[(((srcStride + 1) / 2) * y) + (x >> 1)];
        }
    }
};

const uint8_t* EncodeApp::setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset)
{
    const uint8_t* buf = pFrameData + currentReadOffset;
    currentReadOffset += bufferSize;
    return buf;
}

int32_t EncodeApp::LoadCurrentFrame(uint8_t *outImagePtr, VkSubresourceLayout outImageLayouts[3],
                                    mio::basic_mmap<mio::access_mode::read, uint8_t>& inputVideoMmap,
                                    uint32_t frameIndex,
                                    uint32_t srcWidth, uint32_t srcHeight,
                                    uint32_t srcStride,
                                    VkFormat inputVkFormat)
{
    // infere frame and individual plane sizes from formatInfo
    const VkMpFormatInfo *formatInfo = YcbcrVkFormatInfo(inputVkFormat);

    const uint32_t bytepp = formatInfo->planesLayout.bpp ? 2 : 1;
    uint32_t inputPlaneSizes[VK_MAX_NUM_IMAGE_PLANES_EXT] = {};
    inputPlaneSizes[0] = bytepp * srcStride * srcHeight; // luma plane size
    uint32_t frameSize = inputPlaneSizes[0];      // add luma plane size
    for(uint32_t plane = 1; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        uint32_t stride = srcStride;
        uint32_t height = srcHeight;

        if (formatInfo->planesLayout.secondaryPlaneSubsampledX) { // if subsampled on X divide width by 2
            stride = (srcStride + 1) / 2; // add 1 before division in case width is an odd number
        }

        if (formatInfo->planesLayout.secondaryPlaneSubsampledY) { // if subsampled on Y divide height by 2
            height = (srcHeight + 1) / 2; // add 1 before division in case height is an odd number
        }

        inputPlaneSizes[plane] = bytepp * stride * height; // new plane size
        frameSize += inputPlaneSizes[plane];     // add new plane size
    }

    assert(inputVideoMmap.is_mapped());

    size_t fileOffset = ((uint64_t)frameSize * frameIndex);
    const size_t mappedLength = inputVideoMmap.mapped_length();
    if (mappedLength < (fileOffset + frameSize)) {
        printf("File overflow at frameIndex %d, width %d, height %d, frameSize %d\n",
               frameIndex, srcWidth, srcHeight, frameSize);
        assert(!"Input file overflow");
        return -1;
    }
    const uint8_t* pFrameData = inputVideoMmap.data() + fileOffset;
    size_t currentReadOffset = 0;

    // set plane offset for every plane that was previously read/mapped from file
    const uint8_t* yCbCrInputPtrs[3] = { nullptr, nullptr, nullptr };
    yCbCrInputPtrs[0] = setPlaneOffset(pFrameData, inputPlaneSizes[0], currentReadOffset);
    for(uint32_t plane = 1 ; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        yCbCrInputPtrs[plane] = setPlaneOffset(pFrameData, inputPlaneSizes[plane], currentReadOffset);
    }

    // convertYUVpitchtoNV12, currently only supports 8-bit formats.
    assert(bytepp == 1);
    ConvertYCbCrPitchToNv12(yCbCrInputPtrs[0], yCbCrInputPtrs[1], yCbCrInputPtrs[2],
                            srcStride, outImagePtr, outImageLayouts,
                            srcWidth, srcHeight);

    return 0;
};

VkVideoComponentBitDepthFlagBitsKHR EncodeApp::GetComponentBitDepthFlagBits(uint32_t bpp)
{
    VkVideoComponentBitDepthFlagBitsKHR componentBitDepth;
    switch (bpp) {
    case 8:
        componentBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        break;
    case 10:
        componentBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        break;
    case 12:
        componentBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
        break;
    default:
        componentBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
        break;
    }
    return componentBitDepth;
};


VkVideoChromaSubsamplingFlagBitsKHR EncodeApp::GetChromaSubsamplingFlagBits(uint32_t chromaFormatIDC)
{
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsamplingFlag;
    switch (chromaFormatIDC) {
    case STD_VIDEO_H264_CHROMA_FORMAT_IDC_MONOCHROME:
        chromaSubsamplingFlag = VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
        break;
    case STD_VIDEO_H264_CHROMA_FORMAT_IDC_420:
        chromaSubsamplingFlag = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        break;
    case STD_VIDEO_H264_CHROMA_FORMAT_IDC_422:
        chromaSubsamplingFlag = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
        break;
    case STD_VIDEO_H264_CHROMA_FORMAT_IDC_444:
        chromaSubsamplingFlag = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
        break;
    default:
        chromaSubsamplingFlag = VK_VIDEO_CHROMA_SUBSAMPLING_INVALID_KHR;
        break;
    }
    return chromaSubsamplingFlag;
};

StdVideoH264SequenceParameterSet EncodeApp::GetStdVideoH264SequenceParameterSet(uint32_t width, uint32_t height,
        StdVideoH264SequenceParameterSetVui* pVui)
{
    StdVideoH264SpsFlags spsFlags = {};
    spsFlags.direct_8x8_inference_flag = 1u;
    spsFlags.frame_mbs_only_flag = 1u;
    spsFlags.vui_parameters_present_flag = (pVui == nullptr) ? 0u : 1u;

    const uint32_t mbAlignedWidth = AlignSize(width, H264MbSizeAlignment);
    const uint32_t mbAlignedHeight = AlignSize(height, H264MbSizeAlignment);

    StdVideoH264SequenceParameterSet sps = {};
    sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
    sps.seq_parameter_set_id = 0u;
    sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    sps.bit_depth_luma_minus8 = 0u;
    sps.bit_depth_chroma_minus8 = 0u;
    sps.log2_max_frame_num_minus4 = 0u;
    sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
    sps.max_num_ref_frames = 1u;
    sps.pic_width_in_mbs_minus1 = mbAlignedWidth / H264MbSizeAlignment - 1;
    sps.pic_height_in_map_units_minus1 = mbAlignedHeight / H264MbSizeAlignment - 1;
    sps.flags = spsFlags;
    sps.pSequenceParameterSetVui = pVui;
    sps.frame_crop_right_offset  = mbAlignedWidth - width;
    sps.frame_crop_bottom_offset = mbAlignedHeight - height;

    // This allows for picture order count values in the range [0, 255].
    sps.log2_max_pic_order_cnt_lsb_minus4 = 4u;

    if (sps.frame_crop_right_offset || sps.frame_crop_bottom_offset) {

        sps.flags.frame_cropping_flag = true;

        if (sps.chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420) {
            sps.frame_crop_right_offset >>= 1;
            sps.frame_crop_bottom_offset >>= 1;
        }
    }

    return sps;
}

StdVideoH264PictureParameterSet EncodeApp::GetStdVideoH264PictureParameterSet (void)
{
    StdVideoH264PpsFlags ppsFlags = {};
    ppsFlags.transform_8x8_mode_flag = 1u;
    ppsFlags.constrained_intra_pred_flag = 0u;
    ppsFlags.deblocking_filter_control_present_flag = 1u;
    ppsFlags.entropy_coding_mode_flag = 1u;

    StdVideoH264PictureParameterSet pps = {};
    pps.seq_parameter_set_id = 0u;
    pps.pic_parameter_set_id = 0u;
    pps.num_ref_idx_l0_default_active_minus1 = 0u;
    pps.flags = ppsFlags;

    return pps;
}

VideoSessionParametersInfo::VideoSessionParametersInfo(VkVideoSessionKHR videoSession, StdVideoH264SequenceParameterSet* sps, StdVideoH264PictureParameterSet* pps)
{
    m_videoSession = videoSession;

    m_encodeH264SessionParametersAddInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT;
    m_encodeH264SessionParametersAddInfo.pNext = nullptr;
    m_encodeH264SessionParametersAddInfo.stdSPSCount = 1;
    m_encodeH264SessionParametersAddInfo.pStdSPSs = sps;
    m_encodeH264SessionParametersAddInfo.stdPPSCount = 1;
    m_encodeH264SessionParametersAddInfo.pStdPPSs = pps;

    m_encodeH264SessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT;
    m_encodeH264SessionParametersCreateInfo.pNext = nullptr;
    m_encodeH264SessionParametersCreateInfo.maxStdSPSCount = 1;
    m_encodeH264SessionParametersCreateInfo.maxStdPPSCount = 1;
    m_encodeH264SessionParametersCreateInfo.pParametersAddInfo = &m_encodeH264SessionParametersAddInfo;

    m_encodeSessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    m_encodeSessionParametersCreateInfo.pNext = &m_encodeH264SessionParametersCreateInfo;
    m_encodeSessionParametersCreateInfo.videoSessionParametersTemplate = nullptr;
    m_encodeSessionParametersCreateInfo.videoSession = m_videoSession;
}

int32_t EncodeApp::InitEncoder(EncodeConfig* encodeConfig)
{
    VkResult result = VK_SUCCESS;

    // create profile
    VkVideoCodecOperationFlagBitsKHR videoCodec = (VkVideoCodecOperationFlagBitsKHR)(encodeConfig->codec);
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling = GetChromaSubsamplingFlagBits(encodeConfig->chromaFormatIDC); // VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR
    VkVideoComponentBitDepthFlagBitsKHR lumaBitDepth = GetComponentBitDepthFlagBits(encodeConfig->bpp); // VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoComponentBitDepthFlagBitsKHR chromaBitDepth = GetComponentBitDepthFlagBits(encodeConfig->bpp); // VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_videoProfile = VkVideoCoreProfile(videoCodec, chromaSubsampling, lumaBitDepth, chromaBitDepth, STD_VIDEO_H264_PROFILE_IDC_HIGH);

    if (!VulkanVideoCapabilities::IsCodecTypeSupported(m_vkDevCtx,
                                                       m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                                       videoCodec)) {
        std::cout << "*** The video codec " << VkVideoCoreProfile::CodecToName(videoCodec) << " is not supported! ***" << std::endl;
        assert(!"The video codec is not supported");
        return -1;
    }

    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoEncodeCapabilitiesKHR videoEncodeCapabilities;
    VkVideoEncodeH264CapabilitiesEXT h264EncodeCapabilities;
    result = VulkanVideoCapabilities::GetVideoEncodeCapabilities<VkVideoEncodeH264CapabilitiesEXT, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT>
                                                                (m_vkDevCtx, m_videoProfile,
                                                                 videoCapabilities,
                                                                 videoEncodeCapabilities,
                                                                 h264EncodeCapabilities);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return -1;
    }

    if (m_verbose) {
        std::cout << "\t\t\t" << ("h264") << "encode capabilities: " << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << videoCapabilities.minBitstreamBufferOffsetAlignment << std::endl;
        std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << videoCapabilities.minBitstreamBufferSizeAlignment << std::endl;
        std::cout << "\t\t\t" << "pictureAccessGranularity: " << videoCapabilities.pictureAccessGranularity.width << " x " << videoCapabilities.pictureAccessGranularity.height << std::endl;
        std::cout << "\t\t\t" << "minExtent: " << videoCapabilities.minCodedExtent.width << " x " << videoCapabilities.minCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxExtent: " << videoCapabilities.maxCodedExtent.width  << " x " << videoCapabilities.maxCodedExtent.height << std::endl;
        std::cout << "\t\t\t" << "maxDpbSlots: " << videoCapabilities.maxDpbSlots << std::endl;
        std::cout << "\t\t\t" << "maxActiveReferencePictures: " << videoCapabilities.maxActiveReferencePictures << std::endl;
    }

    VkFormat supportedDpbFormats[8];
    VkFormat supportedInFormats[8];
    uint32_t formatCount = sizeof(supportedDpbFormats) / sizeof(supportedDpbFormats[0]);
    result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, m_videoProfile,
                                                      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                                                      formatCount, supportedDpbFormats);

    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for the decoded picture buffer.\n");
        return -1;
    }

    result = VulkanVideoCapabilities::GetVideoFormats(m_vkDevCtx, m_videoProfile,
                                                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                                                      formatCount, supportedInFormats);

    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for input images.\n");
        return -1;
    }


    m_imageDpbFormat = supportedDpbFormats[0];
    m_imageInFormat = supportedInFormats[0];

    m_maxCodedExtent = { encodeConfig->width, encodeConfig->height }; // codedSize
    m_maxReferencePicturesSlotsCount = DECODED_PICTURE_BUFFER_SIZE;

    VkVideoSessionCreateFlagsKHR sessionCreateFlags{};
#ifdef VK_KHR_video_maintenance1
    m_videoMaintenance1FeaturesSupported = VulkanVideoCapabilities::GetVideoMaintenance1FeatureSupported(m_vkDevCtx);
    if (m_videoMaintenance1FeaturesSupported) {
        sessionCreateFlags |= VK_VIDEO_SESSION_CREATE_INLINE_QUERIES_BIT_KHR;
    }
#endif // VK_KHR_video_maintenance1

    if (!m_videoSession ||
            !m_videoSession->IsCompatible( m_vkDevCtx,
                                           sessionCreateFlags,
                                           m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                           &m_videoProfile,
                                           m_imageInFormat,
                                           m_maxCodedExtent,
                                           m_imageDpbFormat,
                                           m_maxReferencePicturesSlotsCount,
                                           std::max<uint32_t>(m_maxReferencePicturesSlotsCount,
                                                              DECODED_PICTURE_BUFFER_SIZE)) ) {

        result = VulkanVideoSession::Create( m_vkDevCtx,
                                             sessionCreateFlags,
                                             m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(),
                                             &m_videoProfile,
                                             m_imageInFormat,
                                             m_maxCodedExtent,
                                             m_imageDpbFormat,
                                             m_maxReferencePicturesSlotsCount,
                                             std::min<uint32_t>(m_maxReferencePicturesSlotsCount,
                                                                DECODED_PICTURE_BUFFER_SIZE),
                                             m_videoSession);

        // after creating a new video session, we need a codec reset.
        m_resetEncoder = true;
        assert(result == VK_SUCCESS);
    }

    VkExtent2D imageExtent {
        std::max(m_maxCodedExtent.width, videoCapabilities.minCodedExtent.width),
        std::max(m_maxCodedExtent.height, videoCapabilities.minCodedExtent.height)
    };

    m_inputNumFrames = INPUT_FRAME_BUFFER_SIZE;
    m_dpbNumFrames = DECODED_PICTURE_BUFFER_SIZE;

    const VkImageUsageFlags outImageUsage = (VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR |
                                               VK_IMAGE_USAGE_SAMPLED_BIT      | VK_IMAGE_USAGE_STORAGE_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkImageUsageFlags dpbImageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;


    {
        // FIXME: need a separate imageCreateInfo for DPB and input images
        assert(m_imageDpbFormat == m_imageInFormat);
        // FIXME: Vulkan video also supports and multi-layered images
        // and some implementations require image arrays for DPB.
        uint32_t  fullImageSizeForStagingBuffer = 0;
        result = m_videoFrameBuffer.InitFramePool(m_vkDevCtx,
                                                    m_videoProfile.GetProfile(),
                                                    m_inputNumFrames,
                                                    m_imageDpbFormat,
                                                    imageExtent.width,
                                                    imageExtent.height,
                                                    fullImageSizeForStagingBuffer,
                                                    VK_IMAGE_TILING_OPTIMAL,
                                                    outImageUsage | dpbImageUsage,
                                                    m_vkDevCtx->GetVideoEncodeQueueFamilyIdx());

        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: InitImagePool() ret(%d) for m_inputNumFrames(%d)\n", result, m_inputNumFrames);
            return -1;
        }
    }
    // create SPS and PPS
    m_sessionParameters.m_sequenceParameterSet = GetStdVideoH264SequenceParameterSet(encodeConfig->width, encodeConfig->height, nullptr);
    m_sessionParameters.m_pictureParameterSet = GetStdVideoH264PictureParameterSet();

    VideoSessionParametersInfo videoSessionParametersInfo(m_videoSession->GetVideoSession(),
            &m_sessionParameters.m_sequenceParameterSet,
            &m_sessionParameters.m_pictureParameterSet);
    VkVideoSessionParametersCreateInfoKHR* encodeSessionParametersCreateInfo = videoSessionParametersInfo.getVideoSessionParametersInfo();
    result = m_vkDevCtx->CreateVideoSessionParametersKHR(*m_vkDevCtx,
                                                         encodeSessionParametersCreateInfo,
                                                         nullptr,
                                                         &m_sessionParameters.m_encodeSessionParameters);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session parameters.\n");
        return -1;
    }

    return 0;
}

int32_t EncodeApp::InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp)
{
    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.videoSession = m_videoSession->GetVideoSession();
    encodeBeginInfo.videoSessionParameters = m_sessionParameters.m_encodeSessionParameters;

    VkVideoEncodeH264FrameSizeEXT encodeH264FrameSize;
    encodeH264FrameSize.frameISize = 0;

    VkVideoEncodeH264QpEXT encodeH264Qp;
    encodeH264Qp.qpI = qp;

    VkVideoEncodeH264RateControlLayerInfoEXT encodeH264RateControlLayerInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT};
    encodeH264RateControlLayerInfo.useMinQp = VK_TRUE;
    encodeH264RateControlLayerInfo.minQp = encodeH264Qp;
    encodeH264RateControlLayerInfo.useMaxQp = VK_TRUE;
    encodeH264RateControlLayerInfo.maxQp = encodeH264Qp;
    encodeH264RateControlLayerInfo.useMaxFrameSize = VK_TRUE;
    encodeH264RateControlLayerInfo.maxFrameSize = encodeH264FrameSize; 

    VkVideoEncodeRateControlLayerInfoKHR encodeRateControlLayerInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR};
    encodeRateControlLayerInfo.pNext = &encodeH264RateControlLayerInfo;

    VkVideoCodingControlInfoKHR codingControlInfo = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    codingControlInfo.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    codingControlInfo.pNext = &encodeRateControlLayerInfo;

    VkVideoEndCodingInfoKHR encodeEndInfo = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};

    // Reset the video session before first use and apply QP values.
    m_vkDevCtx->CmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);
    m_vkDevCtx->CmdControlVideoCodingKHR(cmdBuf, &codingControlInfo);
    m_vkDevCtx->CmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);

    return 0;
}

// 1. load current input frame from file
// 2. convert yuv image to nv12
// 3. copy nv12 input image to the correct input vkimage slot (staging buffer)
int32_t EncodeApp::LoadFrame(EncodeConfig* encodeConfig, uint32_t frameIndexNum, uint32_t currentFrameBufferIdx)
{
    EncodeFrameData* currentEncodeFrameData = m_videoFrameBuffer.GetEncodeFrameData(currentFrameBufferIdx);
    VkSharedBaseObj<VkImageResourceView>& linearInputImageView = currentEncodeFrameData->m_linearInputImage;

    const VkSharedBaseObj<VkImageResource>& dstImageResource = linearInputImageView->GetImageResource();
    const VkFormat format = dstImageResource->GetImageCreateInfo().format;
    VkSharedBaseObj<VulkanDeviceMemoryImpl> srcImageDeviceMemory(dstImageResource->GetMemory());
    VkImage  srcImage = dstImageResource->GetImage ();

    // Map the image and read the image data.
    VkDeviceSize imageOffset = dstImageResource->GetImageDeviceMemoryOffset();
    VkDeviceSize maxSize = 0;
    uint8_t* writeImagePtr = srcImageDeviceMemory->GetDataPtr(imageOffset, maxSize);
    assert(writeImagePtr != nullptr);

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
    bool isUnnormalizedRgba = false;
    if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
        isUnnormalizedRgba = true;
    }

    VkImageSubresource subResource = {};
    VkSubresourceLayout layouts[3];
    memset(layouts, 0x00, sizeof(layouts));

    if (mpInfo && !isUnnormalizedRgba) {
        switch (mpInfo->planesLayout.layout) {
            case YCBCR_SINGLE_PLANE_UNNORMALIZED:
            case YCBCR_SINGLE_PLANE_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[0]);
                break;
            case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[1]);
                break;
            case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
            case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
            case YCBCR_PLANAR_STRIDE_PADDED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[1]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[2]);
                break;
            default:
                assert(0);
        }

    } else {
        m_vkDevCtx->GetImageSubresourceLayout(*m_vkDevCtx, srcImage, &subResource, &layouts[0]);
    }

    // Load current frame from file and convert to NV12
    LoadCurrentFrame(writeImagePtr, layouts, encodeConfig->inputVideoMmap, frameIndexNum,
                     encodeConfig->width, encodeConfig->height,
                     encodeConfig->width,
                     encodeConfig->inputVkFormat);

    return 0;
}

void EncodeApp::POCBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                         uint8_t& m_refPicMarkingOpCount) {
    int picNumX = -1;
    int currPicNum = -1;
    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);

    picNumX = m_dpb264.GetPicNumXWithMinPOC(0, 0, 0);

    // TODO: Check if this needs to be changed to m_dpb264.GetCurrentPicNum()
    currPicNum = m_dpb264.GetCurrentDpbEntry()->frame_num % maxPicNum;

    if (currPicNum > 0 && (picNumX >= 0)) {
        m_mmco[m_refPicMarkingOpCount].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM;
        m_mmco[m_refPicMarkingOpCount++].difference_of_pic_nums_minus1 = currPicNum - picNumX - 1;
        m_mmco[m_refPicMarkingOpCount++].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END;
    }
}

void EncodeApp::FrameNumBasedRefPicManagement(StdVideoEncodeH264RefPicMarkingEntry* m_mmco,
                                              uint8_t& m_refPicMarkingOpCount)
{
    int picNumX = -1;
    int currPicNum = -1;
    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);

    picNumX = m_dpb264.GetPicNumXWithMinFrameNumWrap(0, 0, 0);

    // TODO: Check if this needs to be changed to m_dpb264.GetCurrentPicNum()
    currPicNum = m_dpb264.GetCurrentDpbEntry()->frame_num % maxPicNum;

    if (currPicNum > 0 && (picNumX >= 0)) {
        m_mmco[m_refPicMarkingOpCount].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_UNMARK_SHORT_TERM;
        m_mmco[m_refPicMarkingOpCount++].difference_of_pic_nums_minus1 = currPicNum - picNumX - 1;
        m_mmco[m_refPicMarkingOpCount++].memory_management_control_operation = STD_VIDEO_H264_MEM_MGMT_CONTROL_OP_END;
    }
}

VkResult EncodeApp::SetupRefPicReorderingCommands(const StdVideoEncodeH264SliceHeader *slh,
                                                  StdVideoEncodeH264ReferenceListsInfoFlags* pFlags,
                                                  StdVideoEncodeH264RefListModEntry* m_ref_pic_list_modification_l0,
                                                  uint8_t& m_refList0ModOpCount)
{
    bool bReorder = false;

    StdVideoEncodeH264RefListModEntry *refPicList0Mod = m_ref_pic_list_modification_l0;

    VkEncDpbEntry entries[MAX_DPB_SIZE];
    memset(entries, 0, sizeof(entries));

    uint32_t numEntries = (uint32_t)m_dpb264.GetValidEntries(entries);
    assert(numEntries <= MAX_DPB_SIZE);

    for (uint32_t i = 0; i < numEntries; i++) {
        if (entries[i].bFrameCorrupted) {
            bReorder = true;
            break;
        }
    }

    // Either the current picture requires no references, or the active
    // reference list does not contain corrupted pictures. Skip reordering.
    if (!bReorder) {
        return VK_SUCCESS;
    }

    NvVideoEncodeH264DpbSlotInfoLists<2 * MAX_REFS> refLists;
    m_dpb264.GetRefPicList(&refLists, &m_h264.m_spsInfo, &m_h264.m_ppsInfo, slh, NULL, true);

    int maxPicNum = 1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4);
    int picNumLXPred = m_dpb264.GetCurrentDpbEntry()->frame_num % maxPicNum;
    int numSTR = 0, numLTR = 0;
    m_dpb264.GetNumRefFramesInDPB(0, &numSTR, &numLTR);

    // Re-order the active list to skip all corrupted frames
    pFlags->ref_pic_list_modification_flag_l0 = true;
    m_refList0ModOpCount = 0;
    if (numSTR) {
        for (uint32_t i = 0; i < refLists.refPicList0Count; i++) {
            int diff = m_dpb264.GetPicNum(refLists.refPicList0[i]) - picNumLXPred;
            if (diff <= 0) {
                refPicList0Mod[m_refList0ModOpCount].modification_of_pic_nums_idc =
                    STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_SUBTRACT;
                refPicList0Mod[m_refList0ModOpCount].abs_diff_pic_num_minus1 = abs(diff) ? abs(diff) - 1 : maxPicNum - 1;
            } else {
                refPicList0Mod[m_refList0ModOpCount].modification_of_pic_nums_idc =
                    STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_SHORT_TERM_ADD;
                refPicList0Mod[m_refList0ModOpCount].abs_diff_pic_num_minus1 = abs(diff) - 1;
            }
            m_refList0ModOpCount++;
            picNumLXPred = m_dpb264.GetPicNum(refLists.refPicList0[i]);
        }
    } else if (numLTR) {
        // If we end up supporting LTR, add code here.
    }

    refPicList0Mod[m_refList0ModOpCount++].modification_of_pic_nums_idc = STD_VIDEO_H264_MODIFICATION_OF_PIC_NUMS_IDC_END;

    assert(m_refList0ModOpCount > 1);

    return VK_SUCCESS;
}

// Generates a mask of slots to be invalidated and frees those slots
void EncodeApp::ResetPicDpbSlot(uint32_t validSlotsMask)
{
    uint32_t resetSlotsMask = ~(validSlotsMask | ~m_dpbSlotsMask);

    if (resetSlotsMask != 0) {
        for (uint32_t referencePictureIndex = 0; referencePictureIndex < m_maxDpbSlots;
                                                                   referencePictureIndex++) {
            if (resetSlotsMask & (1 << referencePictureIndex)) {
                SetPicDpbSlot(referencePictureIndex, -1);
                resetSlotsMask &= ~(1 << referencePictureIndex);
            }
        }
    }
}

// Associate a picture with the current "DPB slot" being occupied by it.
// Set dpbSlot == -1 to indicate that `picIdx' is no longer present in the DPB.
int8_t EncodeApp::SetPicDpbSlot(uint32_t referencePictureIndex, int8_t dpbSlot)
{
    int8_t oldDpbSlot = m_picIdxToDpb[referencePictureIndex];
    m_picIdxToDpb[referencePictureIndex] = dpbSlot;

    if (dpbSlot >= 0) {
        m_dpbSlotsMask |= (1 << referencePictureIndex);
    } else {
        m_dpbSlotsMask &= ~(1 << referencePictureIndex);
    }

    return oldDpbSlot;
}

VkResult EncodeApp::EncodeH264Frame(EncPicParams *pEncPicParams,
                                    EncodeConfig* encodeConfig,
                                    VkCommandBuffer cmdBuf,
                                    uint32_t curFrameIndex,
                                    uint32_t currentFrameBufferIdx,
                                    VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                    VkSharedBaseObj<VkBufferResource>& outBitstream)
{
    // Configuration parameters
    const uint32_t maxReferences = 16;
    const uint32_t maxNumSlices = 64;

    const EncodePerFrameConstConfig* pPerFrameConfig = nullptr;
    if (curFrameIndex == 0) {
        pPerFrameConfig = &encodeConfig->m_firstFrameConfig;
    } else if (pEncPicParams->lastFrame) {
        pPerFrameConfig = &encodeConfig->m_lastFrameConfig;
    } else {
        const uint32_t perFrameConfigIndx = curFrameIndex % encodeConfig->m_perFrameConfigSize;
        pPerFrameConfig = &encodeConfig->m_perFrameConfig[perFrameConfigIndx];
    }

    VkVideoReferenceSlotInfoKHR refSlots[maxReferences];
    StdVideoEncodeH264ReferenceInfo stdReferenceInfo[maxReferences];
    VkVideoEncodeH264DpbSlotInfoEXT dpbSlotInfo[maxReferences];

    VkVideoEncodeH264NaluSliceInfoEXT sliceInfo[maxNumSlices];

    StdVideoH26XPictureType picType = pEncPicParams->pictureType = pPerFrameConfig->m_pictureType;
    const bool refPicFlag = (picType == STD_VIDEO_H26X_PICTURE_TYPE_IDR) ? true :
                            (picType != STD_VIDEO_H26X_PICTURE_TYPE_P) ? true : false;

    bool isIdr = (picType == STD_VIDEO_H26X_PICTURE_TYPE_IDR);
    bool isReference = refPicFlag;

    if (isIdr && (curFrameIndex == 0)) {
        VkResult result = VK_SUCCESS;

        VkVideoEncodeH264SessionParametersGetInfoEXT h264GetInfo = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_EXT,
            nullptr,
            VK_TRUE,
            VK_TRUE,
            m_h264.m_spsInfo.seq_parameter_set_id,
            m_h264.m_ppsInfo.pic_parameter_set_id,
        };

        VkVideoEncodeSessionParametersGetInfoKHR getInfo = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
            &h264GetInfo,
            m_sessionParameters.m_encodeSessionParameters,
        };

        VkVideoEncodeH264SessionParametersFeedbackInfoEXT h264FeedbackInfo = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_EXT,
            nullptr,
        };

        VkVideoEncodeSessionParametersFeedbackInfoKHR feedbackInfo = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
            &h264FeedbackInfo,
        };

        size_t bufferSize = 256;
        result = m_vkDevCtx->GetEncodedVideoSessionParametersKHR(*m_vkDevCtx,
                                                                 &getInfo,
                                                                 &feedbackInfo,
                                                                 &bufferSize,
                                                                 pEncPicParams->bitstreamHeaderBuffer);

        if (result != VK_SUCCESS) {
            return result;
        }
        pEncPicParams->nonVclDataSize = bufferSize;
    }

    DpbPicInfo dpbPicInfo{};
    StdVideoEncodeH264SliceHeader slh{};

    StdVideoEncodeH264RefPicMarkingEntry m_mmco[MAX_MMCOS]{};
    StdVideoEncodeH264RefListModEntry m_ref_pic_list_modification_l0[MAX_REFS]{};
    StdVideoEncodeH264RefListModEntry m_ref_pic_list_modification_l1[MAX_REFS]{};

    uint8_t m_refPicMarkingOpCount = 0;
    // ref_pic_list_modification
    uint8_t m_refList0ModOpCount = 0;
    uint8_t m_refList1ModOpCount = 0;

    m_refPicMarkingOpCount = m_refList0ModOpCount = m_refList1ModOpCount = 0;

    dpbPicInfo.frameNum = m_frameNumSyntax & ((1 << (m_h264.m_spsInfo.log2_max_frame_num_minus4 + 4)) - 1);
    dpbPicInfo.PicOrderCnt = (pEncPicParams->h264.displayPOCSyntax) & ((1 << (m_h264.m_spsInfo.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1);
    dpbPicInfo.pictureType = picType;
    dpbPicInfo.isLongTerm = false;  // TODO: replace this by a check for LONG_TERM_REFERENCE_BIT
    dpbPicInfo.isRef = isReference;
    dpbPicInfo.isIDR = isIdr;
    dpbPicInfo.no_output_of_prior_pics_flag = false;        // TODO: replace this by a check for the corresponding slh flag
    dpbPicInfo.adaptive_ref_pic_marking_mode_flag = false;  // TODO: replace this by a check for the corresponding slh flag
    dpbPicInfo.timeStamp = pEncPicParams->inputTimeStamp;

    int targetFbIndex = m_dpb264.DpbPictureStart(&dpbPicInfo, &m_h264.m_spsInfo);
    uint32_t maxPictureImageIndexInUse = (targetFbIndex > 0) ? targetFbIndex : 0;

    const uint32_t adaptiveRefPicManagementMode = 0; // FIXME
    if ((m_dpb264.GetNumRefFramesInDPB(0) >= m_h264.m_spsInfo.max_num_ref_frames) && isReference &&
        (adaptiveRefPicManagementMode > 0) && !isIdr) {
        // slh.flags.adaptive_ref_pic_marking_mode_flag = true;

        if (adaptiveRefPicManagementMode == 2) {
            POCBasedRefPicManagement(m_mmco, m_refPicMarkingOpCount);
        } else if (adaptiveRefPicManagementMode == 1) {
            FrameNumBasedRefPicManagement(m_mmco, m_refPicMarkingOpCount);
        }
    }

    StdVideoEncodeH264ReferenceListsInfoFlags refMgmtFlags = StdVideoEncodeH264ReferenceListsInfoFlags();
    if ((m_dpb264.IsRefFramesCorrupted()) && ((picType == STD_VIDEO_H26X_PICTURE_TYPE_P) || (picType == STD_VIDEO_H26X_PICTURE_TYPE_B))) {
        SetupRefPicReorderingCommands(&slh, &refMgmtFlags, m_ref_pic_list_modification_l0, m_refList0ModOpCount);
    }

    // Fill in the reference-related information for the current picture
    StdVideoEncodeH264ReferenceListsInfo referenceFinalLists = {};
    referenceFinalLists.flags = refMgmtFlags;
    referenceFinalLists.refPicMarkingOpCount = m_refPicMarkingOpCount;
    referenceFinalLists.refList0ModOpCount = m_refList0ModOpCount;
    referenceFinalLists.refList1ModOpCount = m_refList1ModOpCount;
    referenceFinalLists.pRefList0ModOperations = m_ref_pic_list_modification_l0;
    referenceFinalLists.pRefList1ModOperations = m_ref_pic_list_modification_l1;
    referenceFinalLists.pRefPicMarkingOperations = m_mmco;

    if ((m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1 > 0) &&
            (picType == STD_VIDEO_H26X_PICTURE_TYPE_B)) {
        // do not use multiple references for l0
        slh.flags.num_ref_idx_active_override_flag = true;
        referenceFinalLists.num_ref_idx_l0_active_minus1 = 0;
    }

    NvVideoEncodeH264DpbSlotInfoLists<2 * maxReferences> refLists;
    m_dpb264.GetRefPicList(&refLists, &m_h264.m_spsInfo, &m_h264.m_ppsInfo, &slh, &referenceFinalLists);
    assert(refLists.refPicList0Count <= 8);
    assert(refLists.refPicList1Count <= 8);

    memcpy(referenceFinalLists.RefPicList0, refLists.refPicList0, refLists.refPicList0Count);
    memcpy(referenceFinalLists.RefPicList1, refLists.refPicList1, refLists.refPicList1Count);

    referenceFinalLists.num_ref_idx_l0_active_minus1 = refLists.refPicList0Count > 0 ? refLists.refPicList0Count - 1 : 0;
    referenceFinalLists.num_ref_idx_l1_active_minus1 = refLists.refPicList1Count > 0 ? refLists.refPicList1Count - 1 : 0;

    slh.flags.num_ref_idx_active_override_flag = false;
    if (picType == STD_VIDEO_H26X_PICTURE_TYPE_B) {
        slh.flags.num_ref_idx_active_override_flag =
            ((referenceFinalLists.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1) ||
             (referenceFinalLists.num_ref_idx_l1_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l1_default_active_minus1));
    } else if (picType == STD_VIDEO_H26X_PICTURE_TYPE_P) {
        slh.flags.num_ref_idx_active_override_flag =
            (referenceFinalLists.num_ref_idx_l0_active_minus1 != m_h264.m_ppsInfo.num_ref_idx_l0_default_active_minus1);
    }

    slh.disable_deblocking_filter_idc = encodeConfig->h264.disable_deblocking_filter_idc;

    // FIXME: set cabac_init_idc based on a query
    slh.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;

    StdVideoH264PictureType stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_INVALID;
    switch (picType) {
        case STD_VIDEO_H26X_PICTURE_TYPE_IDR:
            slh.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_IDR;
            break;
        case STD_VIDEO_H26X_PICTURE_TYPE_I:
            slh.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_I;
            break;
        case STD_VIDEO_H26X_PICTURE_TYPE_P:
            slh.slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_P;
            break;
        case STD_VIDEO_H26X_PICTURE_TYPE_B:
            slh.slice_type = STD_VIDEO_H264_SLICE_TYPE_B;
            stdPictureType = STD_VIDEO_H264_PICTURE_TYPE_B;
            break;
        default:
            assert(!"Invalid value");
            break;
    }

    StdVideoEncodeH264PictureInfo currentDpbEntry = *m_dpb264.GetCurrentDpbEntry();
    currentDpbEntry.flags.IdrPicFlag = isIdr;
    currentDpbEntry.flags.is_reference = isReference;
    currentDpbEntry.seq_parameter_set_id = m_h264.m_spsInfo.seq_parameter_set_id;
    currentDpbEntry.pic_parameter_set_id = m_h264.m_ppsInfo.pic_parameter_set_id;
    currentDpbEntry.primary_pic_type = stdPictureType;

    if (isIdr) {
        currentDpbEntry.idr_pic_id = m_IDRPicId & 1;
        m_IDRPicId++;
    }

    uint32_t usedFbSlotsMask = 0;

    VkEncDpbEntry entries[MAX_DPB_SIZE];
    memset(entries, 0, sizeof(entries));

    // Get the valid reference entries to determine indices of in-use pictures
    uint32_t numEntries = (uint32_t)m_dpb264.GetValidEntries(entries);
    assert(numEntries <= MAX_DPB_SIZE);

    for (uint32_t i = 0; i < numEntries; i++) {
        int fbIdx = entries[i].fb_index;

        assert(fbIdx >= 0);
        usedFbSlotsMask |= (1 << fbIdx);
    }

    if (refPicFlag) {
        usedFbSlotsMask |= (1 << targetFbIndex);
    }

    ResetPicDpbSlot(usedFbSlotsMask);

    // We need the reference slot for the target picture
    // Update the DPB
    int8_t targetDpbSlot = m_dpb264.DpbPictureEnd(&m_h264.m_spsInfo, &slh, &referenceFinalLists);
    if (refPicFlag) {
        assert(targetDpbSlot >= 0);
    }

    if ((picType == STD_VIDEO_H26X_PICTURE_TYPE_P) || (picType == STD_VIDEO_H26X_PICTURE_TYPE_B)) {
        currentDpbEntry.pRefLists = &referenceFinalLists;
    }

    memset(refSlots, 0, sizeof(refSlots));
    memset(stdReferenceInfo, 0, sizeof(stdReferenceInfo));
    memset(dpbSlotInfo, 0, sizeof(dpbSlotInfo));

    uint32_t numReferenceSlots = 0;

    if (targetFbIndex >= 0) {
        maxPictureImageIndexInUse = std::max((uint32_t)targetFbIndex, maxPictureImageIndexInUse);

        refSlots[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        refSlots[numReferenceSlots].slotIndex = targetDpbSlot; // m_picIdxToDpb[targetFbIndex];
        //targetReferencePictureIndex = numReferenceSlots;
        refSlots[numReferenceSlots].pPictureResource = &pEncPicParams->refPicList[targetFbIndex];

        numReferenceSlots++;
        assert(numReferenceSlots <= ARRAYSIZE(refSlots));
    }

    // It's not entirely correct to have two separate loops below, one for L0
    // and the other for L1. In each loop, elements are added to refSlots[]
    // without checking for duplication. Duplication could occur if the same
    // picture appears in both L0 and L1; AFAIK, we don't have a situation
    // today like that so the two loops work fine.
    // TODO: create a set out of the ref lists and then iterate over that to
    // build refSlots[].

    for (uint32_t i = 0; i < refLists.refPicList0Count; i++) {
        uint32_t referencePictureIndex = m_dpb264.GetRefPicIdx(refLists.refPicList0[i]);
        assert(referencePictureIndex != uint32_t(-1));

        maxPictureImageIndexInUse = std::max(referencePictureIndex, maxPictureImageIndexInUse);

        m_dpb264.FillStdReferenceInfo(refLists.refPicList0[i],
                                              &stdReferenceInfo[numReferenceSlots]);

        dpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_EXT;
        dpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &stdReferenceInfo[numReferenceSlots];

        refSlots[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        refSlots[numReferenceSlots].pNext = &dpbSlotInfo[numReferenceSlots];
        refSlots[numReferenceSlots].slotIndex = m_picIdxToDpb[referencePictureIndex];
        refSlots[numReferenceSlots].pPictureResource = &pEncPicParams->refPicList[referencePictureIndex];

        numReferenceSlots++;
        assert(numReferenceSlots <= ARRAYSIZE(refSlots));
    }

    for (uint32_t i = 0; i < refLists.refPicList1Count; i++) {
        uint32_t referencePictureIndex = m_dpb264.GetRefPicIdx(refLists.refPicList1[i]);
        assert(referencePictureIndex != uint32_t(-1));

        maxPictureImageIndexInUse = std::max(referencePictureIndex, maxPictureImageIndexInUse);

        m_dpb264.FillStdReferenceInfo(refLists.refPicList1[i],
                                              &stdReferenceInfo[numReferenceSlots]);

        dpbSlotInfo[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_EXT;
        dpbSlotInfo[numReferenceSlots].pStdReferenceInfo = &stdReferenceInfo[numReferenceSlots];

        refSlots[numReferenceSlots].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        refSlots[numReferenceSlots].pNext = &dpbSlotInfo[numReferenceSlots];
        refSlots[numReferenceSlots].slotIndex = m_picIdxToDpb[referencePictureIndex];
        refSlots[numReferenceSlots].pPictureResource = &pEncPicParams->refPicList[referencePictureIndex];

        assert(numReferenceSlots <= ARRAYSIZE(refSlots));
        numReferenceSlots++;
    }

    VkVideoBeginCodingInfoKHR encodeBeginInfo;
    memset(&encodeBeginInfo, 0, sizeof(encodeBeginInfo));
    encodeBeginInfo.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    encodeBeginInfo.videoSession = m_videoSession->GetVideoSession();
    encodeBeginInfo.videoSessionParameters = m_sessionParameters.m_encodeSessionParameters;

    assert((maxPictureImageIndexInUse + 1) <= ARRAYSIZE(pEncPicParams->refPicList));
    encodeBeginInfo.referenceSlotCount = numReferenceSlots;

    // TODO: Order reference slots based on slot # and not referencePictureIndex
    // TODO: This information is currently discarded in the driver.
    encodeBeginInfo.pReferenceSlots = refSlots;

    m_vkDevCtx->CmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);

    m_rcLayerInfo.pNext = & m_h264.m_rcLayerInfoH264;
    m_h264.m_rcInfoH264.temporalLayerCount = 1;

    VkVideoEncodeQualityLevelInfoKHR qualityLevelInfo = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR };
    qualityLevelInfo.qualityLevel = pEncPicParams->qualityLevel;
    qualityLevelInfo.pNext = & m_h264.m_rcInfoH264;

    m_rcInfo.pNext = &qualityLevelInfo;
    m_rcInfo.layerCount = 1;
    m_rcInfo.pLayers = &m_rcLayerInfo;

    if (m_sendControlCmd == true) {
        void *pNext = nullptr;
        VkVideoCodingControlFlagsKHR flags = 0;

        if (m_rateControlTestMode) {
                // Default case
                // Reset Encoder + VkVideoEncodeRateControlInfoKHR
                // Only VkVideoEncodeRateControlInfoKHR
                flags |= VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR |
                         VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
                pNext = &m_rcInfo;
        }

        if (m_sendResetControlCmd == true) {
            flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        }
        VkVideoCodingControlInfoKHR renderControlInfo = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, pNext, flags};
        m_vkDevCtx->CmdControlVideoCodingKHR(cmdBuf, &renderControlInfo);
        m_sendControlCmd = false;
        m_sendResetControlCmd = false;
    }

    VkVideoEncodeInfoKHR encodeInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    encodeInfo.dstBuffer = outBitstream->GetBuffer();

    // For the actual (VCL) data, specify its insertion starting from the
    // provided offset into the bitstream buffer.
    encodeInfo.dstBufferOffset = 0; // pEncPicParams->bitstreamBufferOffset;

    // XXX: We don't really test encoder state reset at the moment.
    // For simplicity, only indicate that the state is to be reset for the
    // first IDR picture.
    // FIXME: The reset must use a RESET control command.
    if (curFrameIndex == 0) {
        encodeInfo.flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    }

    VkVideoReferenceSlotInfoKHR setupReferenceSlot = VkVideoReferenceSlotInfoKHR();
    if (refPicFlag) {
        assert(targetDpbSlot >= 0);
        setupReferenceSlot = refSlots[0];
    }

    encodeInfo.pSetupReferenceSlot = refPicFlag ? &setupReferenceSlot : nullptr;

    // If the current picture is going to be a reference frame, the first
    // entry in the refSlots array contains information about the picture
    // resource associated with this frame. This entry should not be
    // provided in the list of reference resources for the current picture,
    // so skip refSlots[0].
    encodeInfo.referenceSlotCount = 1 /* h264PicParams->refPicFlag */ ? numReferenceSlots - 1 : numReferenceSlots;
    encodeInfo.pReferenceSlots = 1 /* h264PicParams->refPicFlag */ ? refSlots + 1 : refSlots;

    encodeInfo.srcPictureResource.imageViewBinding = srcImageView->GetImageView();

    memset(&sliceInfo, 0, sizeof(sliceInfo));

    sliceInfo[0].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_EXT;
    sliceInfo[0].pStdSliceHeader = &slh;

    if (m_rcInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        switch (picType) {
            case STD_VIDEO_H26X_PICTURE_TYPE_IDR:
            case STD_VIDEO_H26X_PICTURE_TYPE_I:
                sliceInfo[0].constantQp = pEncPicParams->constQp.qpIntra;
                break;
            case STD_VIDEO_H26X_PICTURE_TYPE_P:
                sliceInfo[0].constantQp = pEncPicParams->constQp.qpInterP;
                break;
            case STD_VIDEO_H26X_PICTURE_TYPE_B:
                sliceInfo[0].constantQp = pEncPicParams->constQp.qpInterB;
                break;
            default:
                assert(!"Invalid picture type");
                break;


        }
    }

    for (uint32_t i = 0; i < pEncPicParams->h264.numSlices; i++) {
        sliceInfo[i] = sliceInfo[0];
    }

    VkVideoEncodeH264PictureInfoEXT encodeH264FrameInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_EXT};
    encodeH264FrameInfo.pNext = nullptr;
    encodeH264FrameInfo.naluSliceEntryCount = pEncPicParams->h264.numSlices;
    encodeH264FrameInfo.pNaluSliceEntries = sliceInfo;
    encodeH264FrameInfo.pStdPictureInfo = &currentDpbEntry;

    encodeInfo.pNext = &encodeH264FrameInfo;

    uint32_t querySlotId = currentFrameBufferIdx;

    // Clear the query results
    const uint32_t numQuerySamples = 1;
    VkQueryPool queryPool = m_videoFrameBuffer.GetQueryPool();
    m_vkDevCtx->CmdResetQueryPool(cmdBuf, queryPool, querySlotId, numQuerySamples);

    m_vkDevCtx->CmdBeginQuery(cmdBuf, queryPool, querySlotId, VkQueryControlFlags());

    m_vkDevCtx->CmdEncodeVideoKHR(cmdBuf, &encodeInfo);

    m_vkDevCtx->CmdEndQuery(cmdBuf, queryPool, querySlotId);

    VkVideoEndCodingInfoKHR encodeEndInfo;
    memset(&encodeEndInfo, 0, sizeof(encodeEndInfo));
    encodeEndInfo.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;

    m_vkDevCtx->CmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);


    if (refPicFlag) {
        // Mark the current picture index as in-use.
        SetPicDpbSlot(targetFbIndex, targetDpbSlot);
    }

    if ((picType == STD_VIDEO_H26X_PICTURE_TYPE_P) || (picType == STD_VIDEO_H26X_PICTURE_TYPE_B)) {
        uint64_t timeStamp = m_dpb264.GetPictureTimestamp(refSlots[0].slotIndex);
        m_dpb264.SetCurRefFrameTimeStamp(timeStamp);
    } else {
        m_dpb264.SetCurRefFrameTimeStamp(0);
    }

    assert(m_dpb264.GetNumRefFramesInDPB(0) <= m_h264.m_spsInfo.max_num_ref_frames);

    return VK_SUCCESS;
}

// 4. begin command buffer
// 5. create SPS and PPS
// 6. create encode session parameters
// 7. begin video coding
// 8. if frame = 0 -- encode non vcl data
// 9. encode vcl data
// 10. end video encoding
VkResult EncodeApp::EncodeFrame(EncodeConfig* encodeConfig, uint32_t curFrameIndex, bool nonVcl, uint32_t currentFrameBufferIdx)
{
    // GOP structure config all intra:
    // only using 1 input frame (I) - slot 0
    // only using 1 reference frame - slot 0
    // update POC

    m_videoFrameBuffer.AddRefPic(currentFrameBufferIdx, currentFrameBufferIdx, curFrameIndex);

    EncodeFrameData* currentEncodeFrameData = m_videoFrameBuffer.GetEncodeFrameData(currentFrameBufferIdx);
    VkCommandBuffer cmdBuf = currentEncodeFrameData->m_cmdBufVideoEncode;

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_vkDevCtx->BeginCommandBuffer(cmdBuf, &beginInfo);

    VkSharedBaseObj<VkImageResourceView>& srcImageView = currentEncodeFrameData->m_linearInputImage;
    VkSharedBaseObj<VkImageResourceView>& dstImageView = currentEncodeFrameData->m_inputImageView;

    m_videoFrameBuffer.CopyLinearToOptimalImage(cmdBuf, srcImageView, dstImageView);

    // Begin video coding

    EncPicParams encPicParams{};
    VkResult result = EncodeH264Frame(&encPicParams,
                                      encodeConfig,
                                      cmdBuf,
                                      curFrameIndex,
                                      currentFrameBufferIdx,
                                      dstImageView,
                                      currentEncodeFrameData->m_outBitstreamBuffer);

    m_vkDevCtx->EndCommandBuffer(cmdBuf);

    // reset ref pic
    m_videoFrameBuffer.ReleaseRefPic(currentFrameBufferIdx);

    return result;
}

int32_t EncodeApp::BatchSubmit(uint32_t firstFrameBufferIdx, uint32_t framesInBatch)
{
    if (!(framesInBatch > 0)) {
        return 0;
    }
    const uint32_t maxFramesInBatch = 8;
    assert(framesInBatch <= maxFramesInBatch);
    VkCommandBuffer cmdBuf[maxFramesInBatch];

    for(uint32_t cmdBufIdx = 0; cmdBufIdx < framesInBatch; cmdBufIdx++) {
        EncodeFrameData* currentEncodeFrameData = m_videoFrameBuffer.GetEncodeFrameData(firstFrameBufferIdx + cmdBufIdx);
        cmdBuf[cmdBufIdx] = currentEncodeFrameData->m_cmdBufVideoEncode;
        currentEncodeFrameData->m_frameSubmitted = true;
    }

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr};
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.pWaitDstStageMask = nullptr;
    submitInfo.commandBufferCount = framesInBatch;
    submitInfo.pCommandBuffers = cmdBuf;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = nullptr;

    VkResult result = m_vkDevCtx->MultiThreadedQueueSubmit(VulkanDeviceContext::ENCODE, 0,
                                                           1, &submitInfo, VK_NULL_HANDLE);

    if (result == VK_SUCCESS) {
        return framesInBatch;
    }

    return -1;
}

// 11. gather results
// 12. write results to file
int32_t EncodeApp::AssembleBitstreamData(EncodeConfig* encodeConfig, bool nonVcl, uint32_t currentFrameBufferIdx)
{
    VkResult result = VK_SUCCESS;

    EncodeFrameData* currentEncodeFrameData = m_videoFrameBuffer.GetEncodeFrameData(currentFrameBufferIdx);
    if (!currentEncodeFrameData->m_frameSubmitted) {
        return 0;
    }

    VkSharedBaseObj<VkBufferResource>& outBitstreamBuffer = currentEncodeFrameData->m_outBitstreamBuffer;

    // get output results
    struct nvVideoEncodeStatus {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    };
    nvVideoEncodeStatus encodeResult[2]; // 2nd slot is non vcl data
    memset(&encodeResult, 0, sizeof(encodeResult));

    VkDeviceSize maxSize;
    uint8_t* data = outBitstreamBuffer->GetDataPtr(0, maxSize);

    VkQueryPool queryPool = m_videoFrameBuffer.GetQueryPool();

    uint32_t bitstreamOffset = 0; // necessary non zero value for first frame
    if(nonVcl) {
        // only on frame 0
        bitstreamOffset = NON_VCL_BITSTREAM_OFFSET;
        uint32_t querySlotIdNonVCL = currentFrameBufferIdx + INPUT_FRAME_BUFFER_SIZE;
        result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx, queryPool, querySlotIdNonVCL, 1, sizeof(nvVideoEncodeStatus),
                                       &encodeResult[1], sizeof(nvVideoEncodeStatus), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
        if(result != VK_SUCCESS) {
            fprintf(stderr, "\nRetrieveData Error: Failed to get non vcl query pool results.\n");
            return -1;
        }
        fwrite(data + encodeResult[1].bitstreamStartOffset, 1, encodeResult[1].bitstreamSize, encodeConfig->outputVid);
    }

    uint32_t querySlotIdVCL = currentFrameBufferIdx;
    result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx, queryPool, querySlotIdVCL, 1, sizeof(nvVideoEncodeStatus),
                                   &encodeResult[0], sizeof(nvVideoEncodeStatus), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nRetrieveData Error: Failed to get vcl query pool results.\n");
        return -1;
    }
    fwrite(data + bitstreamOffset + encodeResult[0].bitstreamStartOffset, 1, encodeResult[0].bitstreamSize, encodeConfig->outputVid);

    currentEncodeFrameData->m_frameSubmitted = false;

    return 0;
}

int32_t EncodeApp::DeinitEncoder()
{
    m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::ENCODE, 0);
    m_vkDevCtx->DestroyVideoSessionParametersKHR(*m_vkDevCtx, m_sessionParameters.m_encodeSessionParameters, nullptr);

    m_videoSession = nullptr;
    m_videoFrameBuffer.DeinitReferenceFramePool();
    m_videoFrameBuffer.DeinitFramePool();

    return 0;
}
