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

#include "NvEncodeApp.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

void EncodeApp::convertYUVpitchtoNV12(const uint8_t *yuvLuma, const uint8_t *yuvCb, const uint8_t *yuvCr, uint8_t *nv12Luma,
                                      uint8_t *nv12Chroma, int32_t width, int32_t height, int32_t srcStride, int32_t dstStride)
{
    int32_t x, y;

    for (y = 0; y < height; y++) {
        memcpy(nv12Luma + (dstStride * y), yuvLuma + (srcStride * y), width);
    }

    if (nv12Chroma) {
        for (y = 0; y < (height + 1) / 2; y++) {
            for (x = 0; x < width; x += 2) {
                nv12Chroma[(y * dstStride) + x] = yuvCb[(((srcStride + 1) / 2) * y) + (x >> 1)];
                nv12Chroma[(y * dstStride) + (x + 1)] = yuvCr[(((srcStride + 1) / 2) * y) + (x >> 1)];
            }
        }
    }
};

const uint8_t* EncodeApp::setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset)
{
    const uint8_t* buf = pFrameData + currentReadOffset;
    currentReadOffset += bufferSize;
    return buf;
}

int32_t EncodeApp::loadCurrentFrame(uint8_t *nv12Input[2], mio::basic_mmap<mio::access_mode::read, uint8_t>& inputVideoMmap,
                                    uint32_t frameIndex, uint32_t width, uint32_t height,
                                    uint32_t srcStride, uint32_t dstStride,
                                    VkFormat inputVkFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
{
    uint32_t planeSizes[VK_MAX_NUM_IMAGE_PLANES_EXT] = {};
    const uint8_t *yuvInputTmp[3] = {nullptr, nullptr, nullptr};

    const VkFormat imageFormat = m_imageFormat;
    // infere frame and individual plane sizes from formatInfo
    const VkMpFormatInfo *formatInfo = YcbcrVkFormatInfo(inputVkFormat);

    const uint32_t bytepp = formatInfo->planesLayout.bpp ? 2 : 1;
    planeSizes[0] = bytepp * width * height; // luma plane size
    uint32_t frameSize = planeSizes[0]; // add luma plane size
    for(uint32_t plane = 1 ; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        uint32_t w = 0;
        uint32_t h = 0;
        if(formatInfo->planesLayout.secondaryPlaneSubsampledX) { // if subsampled on X divide width by 2
            w = (width + 1) / 2; // add 1 before division in case width is an odd number
        }
        else {
            w = width;
        }
        if(formatInfo->planesLayout.secondaryPlaneSubsampledY) { // if subsampled on Y divide height by 2
            h = (height + 1) / 2; // add 1 before division in case height is an odd number
        }
        else {
            h = height;
        }
        planeSizes[plane] = bytepp * w * h; // new plane size
        frameSize += planeSizes[plane]; // add new plane size
    }

    assert(inputVideoMmap.is_mapped());

    size_t fileOffset = ((uint64_t)frameSize * frameIndex);
    const size_t mappedLength = inputVideoMmap.mapped_length();
    if (mappedLength < (fileOffset + frameSize)) {
        printf("File overflow at frameIndex %d, width %d, height %d, frameSize %d\n",
               frameIndex, width, height, frameSize);
        assert(!"Input file overflow");
        return -1;
    }
    const uint8_t* pFrameData = inputVideoMmap.data() + fileOffset;
    size_t currentReadOffset = 0;

    // set plane offset for every plane that was previously read/mapped from file
    yuvInputTmp[0] = setPlaneOffset(pFrameData, planeSizes[0], currentReadOffset);
    for(uint32_t plane = 1 ; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        yuvInputTmp[plane] = setPlaneOffset(pFrameData, planeSizes[plane], currentReadOffset);
    }

    // convertYUVpitchtoNV12, currently only supports 8-bit formats.
    assert(bytepp == 1);
    convertYUVpitchtoNV12(yuvInputTmp[0], yuvInputTmp[1], yuvInputTmp[2], nv12Input[0],
                          nv12Input[1], width, height, srcStride, dstStride);

    return 0;
};

VkVideoComponentBitDepthFlagBitsKHR EncodeApp::getComponentBitDepthFlagBits(uint32_t bpp)
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


VkVideoChromaSubsamplingFlagBitsKHR EncodeApp::getChromaSubsamplingFlagBits(uint32_t chromaFormatIDC)
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

VkResult EncodeApp::getVideoFormats(VkPhysicalDevice physicalDevice, VkVideoCoreProfile* pVideoProfile, VkImageUsageFlags imageUsage, uint32_t& formatCount, VkFormat* formats)
{
    for (uint32_t i = 0; i < formatCount; i++) {
        formats[i] = VK_FORMAT_UNDEFINED;
    }

    const VkVideoProfileListInfoKHR videoProfiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, NULL, 1, pVideoProfile->GetProfile() };
    const VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &videoProfiles, imageUsage };

    uint32_t supportedFormatCount = 0;
    VkResult result = vkGetPhysicalDeviceVideoFormatPropertiesKHR(physicalDevice, &videoFormatInfo, &supportedFormatCount, nullptr);
    assert(result == VK_SUCCESS);
    assert(supportedFormatCount);

    VkVideoFormatPropertiesKHR* pSupportedFormats = new VkVideoFormatPropertiesKHR[supportedFormatCount];
    memset(pSupportedFormats, 0x00, supportedFormatCount * sizeof(VkVideoFormatPropertiesKHR));
    for (uint32_t i = 0; i < supportedFormatCount; i++) {
        pSupportedFormats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }

    result = vkGetPhysicalDeviceVideoFormatPropertiesKHR(physicalDevice, &videoFormatInfo, &supportedFormatCount, pSupportedFormats);
    assert(result == VK_SUCCESS);
    std::cout << "\t\t\t" << ("h264") << "encode formats: " << std::endl;
    for (uint32_t fmt = 0; fmt < supportedFormatCount; fmt++) {
        std::cout << "\t\t\t " << fmt << ": " << std::hex << pSupportedFormats[fmt].format << std::dec << std::endl;
    }

    formatCount = std::min(supportedFormatCount, formatCount);

    for (uint32_t i = 0; i < formatCount; i++) {
        formats[i] = pSupportedFormats[i].format;
    }

    delete[] pSupportedFormats;

    return result;
}

VkResult EncodeApp::getVideoCapabilities(VkPhysicalDevice physicalDevice, VkVideoCoreProfile* pVideoProfile, VkVideoCapabilitiesKHR* pVideoCapabilities)
{
    VkVideoEncodeCapabilitiesKHR* pVideoEncodeCapabilities = nullptr;
    VkVideoEncodeH264CapabilitiesEXT* pH264Capabilities = nullptr;

    assert(pVideoCapabilities->sType == VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
    assert(pVideoCapabilities->pNext);

    pVideoEncodeCapabilities = (VkVideoEncodeCapabilitiesKHR*)pVideoCapabilities->pNext;

    if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT) {
        assert(pVideoEncodeCapabilities->pNext);
        pH264Capabilities = (VkVideoEncodeH264CapabilitiesEXT*)pVideoEncodeCapabilities->pNext;
        assert(pH264Capabilities->sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT);
    }
    else {
        assert(!"Unsupported codec");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkResult result = vkGetPhysicalDeviceVideoCapabilitiesKHR(physicalDevice, pVideoProfile->GetProfile(), pVideoCapabilities);
    assert(result == VK_SUCCESS);

    std::cout << "\t\t\t" << ("h264") << "encode capabilities: " << std::endl;
    std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << pVideoCapabilities->minBitstreamBufferOffsetAlignment << std::endl;
    std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << pVideoCapabilities->minBitstreamBufferSizeAlignment << std::endl;
    std::cout << "\t\t\t" << "pictureAccessGranularity: " << pVideoCapabilities->pictureAccessGranularity.width << " x " << pVideoCapabilities->pictureAccessGranularity.height << std::endl;
    std::cout << "\t\t\t" << "minExtent: " << pVideoCapabilities->minCodedExtent.width << " x " << pVideoCapabilities->minCodedExtent.height << std::endl;
    std::cout << "\t\t\t" << "maxExtent: " << pVideoCapabilities->maxCodedExtent.width  << " x " << pVideoCapabilities->maxCodedExtent.height << std::endl;
    std::cout << "\t\t\t" << "maxDpbSlots: " << pVideoCapabilities->maxDpbSlots << std::endl;
    std::cout << "\t\t\t" << "maxActiveReferencePictures: " << pVideoCapabilities->maxActiveReferencePictures << std::endl;

    if (pVideoProfile->GetCodecType() == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT) {
        if (strncmp(pVideoCapabilities->stdHeaderVersion.extensionName,
                    VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
                    sizeof (pVideoCapabilities->stdHeaderVersion.extensionName) - 1U) ||
                (pVideoCapabilities->stdHeaderVersion.specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION)) {
            assert(!"Unsupported h.264 STD version");
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
    }
    else {
        assert(!"Unsupported codec");
    }

    return result;
}

StdVideoH264SequenceParameterSet EncodeApp::getStdVideoH264SequenceParameterSet(uint32_t width, uint32_t height,
        StdVideoH264SequenceParameterSetVui* pVui)
{
    StdVideoH264SpsFlags spsFlags = {};
    spsFlags.direct_8x8_inference_flag = 1u;
    spsFlags.frame_mbs_only_flag = 1u;
    spsFlags.vui_parameters_present_flag = (pVui == NULL) ? 0u : 1u;

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

StdVideoH264PictureParameterSet EncodeApp::getStdVideoH264PictureParameterSet (void)
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


IntraFrameInfo::IntraFrameInfo(uint32_t frameCount, uint32_t width, uint32_t height, StdVideoH264SequenceParameterSet sps, StdVideoH264PictureParameterSet pps, bool isIdr)
{
    const uint32_t MaxPicOrderCntLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);

    m_sliceHeaderFlags.num_ref_idx_active_override_flag = 0;
    m_sliceHeaderFlags.no_output_of_prior_pics_flag = 0;
    m_sliceHeaderFlags.adaptive_ref_pic_marking_mode_flag = 0;
    m_sliceHeaderFlags.no_prior_references_available_flag = 0;

    m_sliceHeader.flags = m_sliceHeaderFlags;
    m_sliceHeader.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
    m_sliceHeader.idr_pic_id = 0;
    m_sliceHeader.num_ref_idx_l0_active_minus1 = 0;
    m_sliceHeader.num_ref_idx_l1_active_minus1 = 0;
    m_sliceHeader.cabac_init_idc = (StdVideoH264CabacInitIdc)0;
    m_sliceHeader.disable_deblocking_filter_idc = (StdVideoH264DisableDeblockingFilterIdc)0;
    m_sliceHeader.slice_alpha_c0_offset_div2 = 0;
    m_sliceHeader.slice_beta_offset_div2 = 0;

    uint32_t picWidthInMbs = sps.pic_width_in_mbs_minus1 + 1;
    uint32_t picHeightInMbs = sps.pic_height_in_map_units_minus1 + 1;
    uint32_t iPicSizeInMbs = picWidthInMbs * picHeightInMbs;

    m_sliceInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_EXT;
    m_sliceInfo.pNext = NULL;
    m_sliceInfo.pSliceHeaderStd = &m_sliceHeader;
    m_sliceInfo.mbCount = iPicSizeInMbs;

    if (isIdr) {
        m_pictureInfoFlags.idr_flag = 1;
        m_pictureInfoFlags.is_reference_flag = 1;
    }

    m_stdPictureInfo.flags = m_pictureInfoFlags;
    m_stdPictureInfo.seq_parameter_set_id = 0;
    m_stdPictureInfo.pic_parameter_set_id = pps.pic_parameter_set_id;
    m_stdPictureInfo.pictureType = STD_VIDEO_H264_PICTURE_TYPE_I;

    // frame_num is incremented for each reference frame transmitted.
    // In our case, only the first frame (which is IDR) is a reference
    // frame with frame_num == 0, and all others have frame_num == 1.
    m_stdPictureInfo.frame_num = isIdr ? 0 : 1;

    // POC is incremented by 2 for each coded frame.
    m_stdPictureInfo.PicOrderCnt = (frameCount * 2) % MaxPicOrderCntLsb;

    m_encodeH264FrameInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_VCL_FRAME_INFO_EXT;
    m_encodeH264FrameInfo.pNext = NULL;
    m_encodeH264FrameInfo.naluSliceEntryCount = 1;
    m_encodeH264FrameInfo.pNaluSliceEntries = &m_sliceInfo;
    m_encodeH264FrameInfo.pCurrentPictureInfo = &m_stdPictureInfo;
}

VideoSessionParametersInfo::VideoSessionParametersInfo(VkVideoSessionKHR videoSession, StdVideoH264SequenceParameterSet* sps, StdVideoH264PictureParameterSet* pps)
{
    m_videoSession = videoSession;

    m_encodeH264SessionParametersAddInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT;
    m_encodeH264SessionParametersAddInfo.pNext = NULL;
    m_encodeH264SessionParametersAddInfo.stdSPSCount = 1;
    m_encodeH264SessionParametersAddInfo.pStdSPSs = sps;
    m_encodeH264SessionParametersAddInfo.stdPPSCount = 1;
    m_encodeH264SessionParametersAddInfo.pStdPPSs = pps;

    m_encodeH264SessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT;
    m_encodeH264SessionParametersCreateInfo.pNext = NULL;
    m_encodeH264SessionParametersCreateInfo.maxStdSPSCount = 1;
    m_encodeH264SessionParametersCreateInfo.maxStdPPSCount = 1;
    m_encodeH264SessionParametersCreateInfo.pParametersAddInfo = &m_encodeH264SessionParametersAddInfo;

    m_encodeSessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    m_encodeSessionParametersCreateInfo.pNext = &m_encodeH264SessionParametersCreateInfo;
    m_encodeSessionParametersCreateInfo.videoSessionParametersTemplate = NULL;
    m_encodeSessionParametersCreateInfo.videoSession = m_videoSession;
}

int32_t EncodeApp::selectNvidiaGPU(std::vector<uint32_t> compatibleDevices, nvvk::ContextCreateInfo ctxInfo, uint32_t deviceID = 0)
{
    uint32_t nbElems;
    std::vector<VkPhysicalDeviceGroupProperties> groups;
    std::vector<VkPhysicalDevice>                physicalDevices;

    if(ctxInfo.useDeviceGroups) {
        groups  = m_ctx.getPhysicalDeviceGroups();
        nbElems = static_cast<uint32_t>(groups.size());
    }
    else {
        physicalDevices = m_ctx.getPhysicalDevices();
        nbElems         = static_cast<uint32_t>(physicalDevices.size());
    }

    for(auto deviceIndex : compatibleDevices) {
        VkPhysicalDevice physicalDevice = ctxInfo.useDeviceGroups ? groups[deviceIndex].physicalDevices[0] : physicalDevices[deviceIndex];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        if(!deviceID) {
            if(props.vendorID == 0x10DE) { // Nvidia vendor ID
                return deviceIndex;
            }
        }
        else {
            if(props.deviceID == deviceID) { // specified device ID
                return deviceIndex;
            }
        }
    }
    return -1;
}

int32_t EncodeApp::initEncoder(EncodeConfig* encodeConfig)
{
    VkResult result = VK_SUCCESS;

    nvvk::ContextCreateInfo ctxInfo;
    // Add all the required device extensions
    ctxInfo.addDeviceExtension(VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME, false);
    ctxInfo.addDeviceExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, false); // req by video
    ctxInfo.addDeviceExtension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, false);
    ctxInfo.addDeviceExtension(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, false);
    ctxInfo.addDeviceExtension(VK_EXT_VIDEO_ENCODE_H264_EXTENSION_NAME, false);
    ctxInfo.removeInstanceLayer("VK_LAYER_KHRONOS_validation"); // may need to disable validation layers

    ctxInfo.addRequestedQueue(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, 1, 1.0f);
    // Checks required/available vulkan version
    // Checks available instance layers and extensions
    // Checks used instance layers and extensions
    // Creates vulkan instance
    m_ctx.initInstance(ctxInfo);

    // Find all physical devices compatible with the mandatory extensions
    // Look for devices that support vulkan video
    std::vector<uint32_t> compatibleDevices = m_ctx.getCompatibleDevices(ctxInfo);

    // Check if at least one compatible physical device is available
    if(compatibleDevices.empty()) {
        fprintf(stderr, "\nInitEncoder Error: Failed to find any compatible devices.\n");
        return -1;
    }

    // From the detected compatible devices pick the first Nvidia one (if available)
    uint32_t nvidiaCompatibleDevice = selectNvidiaGPU(compatibleDevices, ctxInfo);
    if(nvidiaCompatibleDevice < 0) {
        fprintf(stderr, "\nInitEncoder Error: Failed to find an Nvidia compatible device.\n");
        return -1;
    }

    // Init selected Nvidia device
    m_ctx.initDevice(nvidiaCompatibleDevice, ctxInfo);

    // Create queue for video encoding
    m_queue = m_ctx.createQueue(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "q_encode", 1.0f);

    // Command Buffer Pool Generator

    m_cmdPoolVideoEncode.init(m_ctx.m_device, m_queue.familyIndex,
                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                              VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);  // Flags: to use command buffer for several recordings

    // create profile
    VkVideoCodecOperationFlagBitsKHR videoCodec = (VkVideoCodecOperationFlagBitsKHR)(encodeConfig->codec);
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling = getChromaSubsamplingFlagBits(encodeConfig->chromaFormatIDC); // VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR
    VkVideoComponentBitDepthFlagBitsKHR lumaBitDepth = getComponentBitDepthFlagBits(encodeConfig->bpp); // VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoComponentBitDepthFlagBitsKHR chromaBitDepth = getComponentBitDepthFlagBits(encodeConfig->bpp); // VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_videoProfile = VkVideoCoreProfile(videoCodec, chromaSubsampling, lumaBitDepth, chromaBitDepth, STD_VIDEO_H264_PROFILE_IDC_HIGH);

    // get supported input formats for encoder & recon images format (dpb)
    VkFormat supportedReconstructedPicturesFormats[4];
    VkFormat supportedInputFormats[4];
    uint32_t formatCountIn = sizeof(supportedInputFormats) / sizeof(supportedInputFormats[0]);
    uint32_t formatCountRecon = sizeof(supportedReconstructedPicturesFormats) / sizeof(supportedReconstructedPicturesFormats[0]);

    result = getVideoFormats(m_ctx.m_physicalDevice, &m_videoProfile, (VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR), formatCountIn, supportedInputFormats);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for input images.\n");
        return -1;
    }

    result = getVideoFormats(m_ctx.m_physicalDevice, &m_videoProfile, (VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR), formatCountRecon, supportedReconstructedPicturesFormats);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video format for the decoded picture buffer.\n");
        return -1;
    }

    // find capabilities of encoder - generic capabilities / h264
    VkVideoCapabilitiesKHR videoCapabilities = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, NULL };
    VkVideoEncodeCapabilitiesKHR videoEncodeCapabilities = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, NULL };
    VkVideoEncodeH264CapabilitiesEXT h264Capabilities = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT, NULL };
    videoCapabilities.pNext = &videoEncodeCapabilities;
    videoEncodeCapabilities.pNext = &h264Capabilities;

    result = getVideoCapabilities(m_ctx.m_physicalDevice, &m_videoProfile, &videoCapabilities);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get desired video capabilities.\n");
        return -1;
    }

    // get codec VkFormat
    bool isSemiPlanar = chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
    m_imageFormat = m_videoProfile.CodecGetVkFormat(chromaSubsampling, lumaBitDepth, isSemiPlanar);
    if(supportedReconstructedPicturesFormats[0] != m_imageFormat) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get codec VkFormat.\n");
        return -1;
    }

    // create and initialize device allocator
    m_devAlloc.init(m_ctx.m_device, m_ctx.m_physicalDevice);
    m_maxCodedExtent = { encodeConfig->width, encodeConfig->height }; // codedSize
    m_maxReferencePicturesSlotsCount = DECODED_PICTURE_BUFFER_SIZE;

    // start video coding session
    result = NvVideoSession::create(&m_devAlloc,
                                    &m_ctx,
                                    m_queue.familyIndex,
                                    &m_videoProfile,
                                    m_imageFormat,
                                    m_maxCodedExtent,
                                    m_imageFormat,
                                    m_maxReferencePicturesSlotsCount,
                                    m_maxReferencePicturesSlotsCount,
                                    &m_pVideoSession);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nInitEncoder Error: Failed to get create video coding session.\n");
        return -1;
    }

    m_inputNumFrames = INPUT_FRAME_BUFFER_SIZE;
    m_dpbNumFrames = DECODED_PICTURE_BUFFER_SIZE;
    m_resAlloc.init(m_ctx.m_device, m_ctx.m_physicalDevice);
    // init input frame pool
    m_pictureBuffer.initFramePool(  &m_ctx,
                                    m_videoProfile.GetProfile(),  // query pool is done here
                                    m_inputNumFrames,
                                    m_imageFormat,
                                    encodeConfig->alignedWidth,
                                    encodeConfig->alignedHeight,
                                    encodeConfig->fullImageSize,
                                    VK_IMAGE_TILING_OPTIMAL,
                                    VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                                    &m_resAlloc,
                                    &m_cmdPoolVideoEncode,
                                    m_queue.familyIndex);
    // init DPB pool
    m_pictureBuffer.initReferenceFramePool(m_dpbNumFrames,
                                           m_imageFormat,
                                           &m_resAlloc);

    // create SPS and PPS
    m_videoSessionParameters.m_sequenceParameterSet = getStdVideoH264SequenceParameterSet(encodeConfig->width, encodeConfig->height, NULL);
    m_videoSessionParameters.m_pictureParameterSet = getStdVideoH264PictureParameterSet();

    VideoSessionParametersInfo videoSessionParametersInfo(m_pVideoSession->getVideoSession(),
            &m_videoSessionParameters.m_sequenceParameterSet,
            &m_videoSessionParameters.m_pictureParameterSet);
    VkVideoSessionParametersCreateInfoKHR* encodeSessionParametersCreateInfo = videoSessionParametersInfo.getVideoSessionParametersInfo();
    result = vkCreateVideoSessionParametersKHR(m_ctx.m_device, encodeSessionParametersCreateInfo, NULL, &m_videoSessionParameters.m_encodeSessionParameters);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nEncodeFrame Error: Failed to get create video session parameters.\n");
        return -1;
    }

    VkCommandBuffer cmdBuf = m_cmdPoolVideoEncode.createCommandBuffer();

    initRateControl(cmdBuf, encodeConfig->qp);

    // Set the layout for images in the input image pool
    m_pictureBuffer.prepareInputImages(cmdBuf);

    // Set the layout for images in the reference image pool
    m_pictureBuffer.prepareReferenceImages(cmdBuf);

    m_cmdPoolVideoEncode.submitAndWait(cmdBuf);

    return 0;
}

int32_t EncodeApp::initRateControl(VkCommandBuffer cmdBuf, uint32_t qp)
{
    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.videoSession = m_pVideoSession->getVideoSession();;
    encodeBeginInfo.videoSessionParameters = m_videoSessionParameters.m_encodeSessionParameters;

    VkVideoEncodeH264FrameSizeEXT encodeH264FrameSize;
    encodeH264FrameSize.frameISize = 0;

    VkVideoEncodeH264QpEXT encodeH264Qp;
    encodeH264Qp.qpI = qp;

    VkVideoEncodeH264RateControlLayerInfoEXT encodeH264RateControlLayerInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT};
    encodeH264RateControlLayerInfo.useInitialRcQp = VK_TRUE;
    encodeH264RateControlLayerInfo.initialRcQp = encodeH264Qp;
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
    vkCmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);
    vkCmdControlVideoCodingKHR(cmdBuf, &codingControlInfo);
    vkCmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);

    return 0;
}

// 1. load current input frame from file
// 2. convert yuv image to nv12
// 3. copy nv12 input image to the correct input vkimage slot (staging buffer)
int32_t EncodeApp::loadFrame(EncodeConfig* encodeConfig, uint32_t frameCount, uint32_t currentFrameBufferIdx)
{
    EncodeFrameData* currentEncodeFrameData = m_pictureBuffer.getEncodeFrameData(currentFrameBufferIdx);
    VkImage inputImage = currentEncodeFrameData->m_picture.m_image.image;
    nvvk::Buffer inputStagingBuffer = currentEncodeFrameData->m_inputStagingBuffer;
    VkCommandBuffer cmdBuf = currentEncodeFrameData->m_cmdBufVideoEncode;
    // map Vkbuffer to uint8_t pointer so the input image can be copied (Host visible)
    uint8_t* stagingBuffer = reinterpret_cast<uint8_t*>(m_resAlloc.map(inputStagingBuffer));
    uint8_t* currentFrame[2];
    currentFrame[0] = stagingBuffer;
    currentFrame[1] = currentFrame[0] + encodeConfig->lumaPlaneSize;

    // Load current frame from file and convert to NV12
    loadCurrentFrame(currentFrame, encodeConfig->inputVideoMmap, frameCount,
                     encodeConfig->width, encodeConfig->height,
                     encodeConfig->width, encodeConfig->alignedWidth,
                     encodeConfig->inputVkFormat);

    m_resAlloc.unmap(inputStagingBuffer);
    return 0;
}

// 4. begin command buffer
// 5. create SPS and PPS
// 6. create encode session parameters
// 7. begin video coding
// 8. if frame = 0 -- encode non vcl data
// 9. encode vcl data
// 10. end video encoding
int32_t EncodeApp::encodeFrame(EncodeConfig* encodeConfig, uint32_t frameCount, bool nonVcl, uint32_t currentFrameBufferIdx)
{
    VkResult result = VK_SUCCESS;

    // GOP structure config all intra:
    // only using 1 input frame (I) - slot 0
    // only using 1 reference frame - slot 0
    // update POC

    m_pictureBuffer.addRefPic(currentFrameBufferIdx, currentFrameBufferIdx, frameCount);

    EncodeFrameData* currentEncodeFrameData = m_pictureBuffer.getEncodeFrameData(currentFrameBufferIdx);
    VkBuffer outBitstream = currentEncodeFrameData->m_outBitstreamBuffer.buffer;
    VkCommandBuffer cmdBuf = currentEncodeFrameData->m_cmdBufVideoEncode;

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    m_pictureBuffer.copyToVkImage(currentFrameBufferIdx, 0, cmdBuf);

    // Begin video coding

    VkQueryPool queryPool = m_pictureBuffer.getQueryPool();
    // query slot id for VCL                   - slots [0,                          1,                         ... INPUT_FRAME_BUFFER_SIZE-1]
    // query slot id for correspondent non VCL - slots [0+INPUT_FRAME_BUFFER_SIZE,  1+INPUT_FRAME_BUFFER_SIZE, ... INPUT_FRAME_BUFFER_SIZE+INPUT_FRAME_BUFFER_SIZE-1]
    uint32_t querySlotIdVCL = currentFrameBufferIdx;
    uint32_t querySlotIdNonVCL = currentFrameBufferIdx + INPUT_FRAME_BUFFER_SIZE;

    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    encodeBeginInfo.videoSession = m_pVideoSession->getVideoSession();;
    encodeBeginInfo.videoSessionParameters = m_videoSessionParameters.m_encodeSessionParameters;
    encodeBeginInfo.referenceSlotCount = 0;
    encodeBeginInfo.pReferenceSlots = NULL;

    vkCmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);

    uint32_t bitstreamOffset = 0; // necessary non zero value for first frame
    if(nonVcl) {
        // Encode Non VCL data - SPS and PPS
        EncodeInfoNonVcl encodeInfoNonVcl(&m_videoSessionParameters.m_sequenceParameterSet,
                                          &m_videoSessionParameters.m_pictureParameterSet,
                                          &outBitstream);
        VkVideoEncodeInfoKHR* videoEncodeInfoNonVcl = encodeInfoNonVcl.getVideoEncodeInfo();
        vkCmdResetQueryPool(cmdBuf, queryPool, querySlotIdNonVCL, 1);
        vkCmdBeginQuery(cmdBuf, queryPool, querySlotIdNonVCL, VkQueryControlFlags());
        vkCmdEncodeVideoKHR(cmdBuf, videoEncodeInfoNonVcl);
        vkCmdEndQuery(cmdBuf, queryPool, querySlotIdNonVCL);
        bitstreamOffset = NON_VCL_BITSTREAM_OFFSET; // use 4k for first frame and then update with size of last frame
    }
    // Encode Frame
    // encode info for vkCmdEncodeVideoKHR
    IntraFrameInfo intraFrameInfo(frameCount, encodeConfig->width, encodeConfig->height,
                                  m_videoSessionParameters.m_sequenceParameterSet,
                                  m_videoSessionParameters.m_pictureParameterSet,
                                  frameCount == 0);
    VkVideoEncodeH264VclFrameInfoEXT* encodeH264FrameInfo = intraFrameInfo.getEncodeH264FrameInfo();

    VkVideoPictureResourceInfoKHR inputPicResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    VkVideoPictureResourceInfoKHR dpbPicResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    m_pictureBuffer.getFrameResourcesByIndex(currentFrameBufferIdx, &inputPicResource);
    m_pictureBuffer.getReferenceFrameResourcesByIndex(currentFrameBufferIdx, &dpbPicResource);

    EncodeInfoVcl encodeInfoVcl(&outBitstream, bitstreamOffset, encodeH264FrameInfo, &inputPicResource, &dpbPicResource);
    VkVideoEncodeInfoKHR* videoEncodeInfoVcl = encodeInfoVcl.getVideoEncodeInfo();

    vkCmdResetQueryPool(cmdBuf, queryPool, querySlotIdVCL, 1);
    vkCmdBeginQuery(cmdBuf, queryPool, querySlotIdVCL, VkQueryControlFlags());
    vkCmdEncodeVideoKHR(cmdBuf, videoEncodeInfoVcl);
    vkCmdEndQuery(cmdBuf, queryPool, querySlotIdVCL);

    VkVideoEndCodingInfoKHR encodeEndInfo = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);
    vkEndCommandBuffer(cmdBuf);

    // reset ref pic
    m_pictureBuffer.removeRefPic(currentFrameBufferIdx);

    return 0;
}

int32_t EncodeApp::batchSubmit(uint32_t firstFrameBufferIdx, uint32_t framesInBatch)
{
    if (!(framesInBatch > 0)) {
        return 0;
    }
    const uint32_t maxFramesInBatch = 8;
    assert(framesInBatch <= maxFramesInBatch);
    VkCommandBuffer cmdBuf[maxFramesInBatch];

    for(uint32_t cmdBufIdx = 0; cmdBufIdx < framesInBatch; cmdBufIdx++) {
        EncodeFrameData* currentEncodeFrameData = m_pictureBuffer.getEncodeFrameData(firstFrameBufferIdx + cmdBufIdx);
        cmdBuf[cmdBufIdx] = currentEncodeFrameData->m_cmdBufVideoEncode;
        currentEncodeFrameData->m_frameSubmitted = true;
    }

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL};
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = NULL;
    submitInfo.pWaitDstStageMask = NULL;
    submitInfo.commandBufferCount = framesInBatch;
    submitInfo.pCommandBuffers = cmdBuf;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = NULL;

    VkResult result = vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);

    if (result == VK_SUCCESS) {
        return framesInBatch;
    }

    return -1;
}

// 11. gather results
// 12. write results to file
int32_t EncodeApp::assembleBitstreamData(EncodeConfig* encodeConfig, bool nonVcl, uint32_t currentFrameBufferIdx)
{
    VkResult result = VK_SUCCESS;

    EncodeFrameData* currentEncodeFrameData = m_pictureBuffer.getEncodeFrameData(currentFrameBufferIdx);
    if (!currentEncodeFrameData->m_frameSubmitted) {
        return 0;
    }

    nvvk::Buffer outBitstreamBuffer = currentEncodeFrameData->m_outBitstreamBuffer;

    // get output results
    struct nvVideoEncodeStatus {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    };
    nvVideoEncodeStatus encodeResult[2]; // 2nd slot is non vcl data
    memset(&encodeResult, 0, sizeof(encodeResult));

    int8_t* data = reinterpret_cast<int8_t*>(m_resAlloc.map(outBitstreamBuffer));

    VkQueryPool queryPool = m_pictureBuffer.getQueryPool();

    uint32_t bitstreamOffset = 0; // necessary non zero value for first frame
    if(nonVcl) {
        // only on frame 0
        bitstreamOffset = NON_VCL_BITSTREAM_OFFSET;
        uint32_t querySlotIdNonVCL = currentFrameBufferIdx + INPUT_FRAME_BUFFER_SIZE;
        result = vkGetQueryPoolResults(m_ctx.m_device, queryPool, querySlotIdNonVCL, 1, sizeof(nvVideoEncodeStatus),
                                       &encodeResult[1], sizeof(nvVideoEncodeStatus), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
        if(result != VK_SUCCESS) {
            fprintf(stderr, "\nRetrieveData Error: Failed to get non vcl query pool results.\n");
            return -1;
        }
        fwrite(data + encodeResult[1].bitstreamStartOffset, 1, encodeResult[1].bitstreamSize, encodeConfig->outputVid);
    }

    uint32_t querySlotIdVCL = currentFrameBufferIdx;
    result = vkGetQueryPoolResults(m_ctx.m_device, queryPool, querySlotIdVCL, 1, sizeof(nvVideoEncodeStatus),
                                   &encodeResult[0], sizeof(nvVideoEncodeStatus), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
    if(result != VK_SUCCESS) {
        fprintf(stderr, "\nRetrieveData Error: Failed to get vcl query pool results.\n");
        return -1;
    }
    fwrite(data + bitstreamOffset + encodeResult[0].bitstreamStartOffset, 1, encodeResult[0].bitstreamSize, encodeConfig->outputVid);

    m_resAlloc.unmap(outBitstreamBuffer);

    currentEncodeFrameData->m_frameSubmitted = false;

    return 0;
}



int32_t EncodeApp::deinitEncoder()
{
    vkQueueWaitIdle(m_queue);
    vkDestroyVideoSessionParametersKHR(m_ctx.m_device, m_videoSessionParameters.m_encodeSessionParameters, NULL);

    delete m_pVideoSession;
    m_pictureBuffer.deinitReferenceFramePool();
    m_pictureBuffer.deinitFramePool();
    m_resAlloc.deinit();
    m_devAlloc.deinit();
    m_cmdPoolVideoEncode.deinit();
    m_ctx.deinit();

    return 0;
}
