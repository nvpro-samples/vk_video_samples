/*
 * Copyright 2018-2023 NVIDIA Corporation.
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

#ifndef _VULKANVIDEOCAPABILITIES_H_
#define _VULKANVIDEOCAPABILITIES_H_

#include "string.h"
#include "vulkan_interfaces.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/Helpers.h"
#include "VkVideoCore/VkVideoCoreProfile.h"

class VulkanVideoCapabilities
{
public:
    static VkResult GetVideoDecodeCapabilities(const VulkanDeviceContext* vkDevCtx,
                                               const VkVideoCoreProfile& videoProfile,
                                               VkVideoCapabilitiesKHR& videoCapabilities,
                                               VkVideoDecodeCapabilitiesKHR& videoDecodeCapabilities) {

        VkVideoCodecOperationFlagsKHR videoCodec = videoProfile.GetProfile()->videoCodecOperation;

        videoDecodeCapabilities = VkVideoDecodeCapabilitiesKHR { VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, nullptr };
        videoCapabilities       =       VkVideoCapabilitiesKHR { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &videoDecodeCapabilities };
        VkVideoDecodeH264CapabilitiesKHR h264Capabilities    = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR, nullptr };
        VkVideoDecodeH265CapabilitiesKHR h265Capabilities    = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR, nullptr };
        VkVideoDecodeAV1CapabilitiesKHR av1Capabilities{};
        av1Capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;

        if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
            videoDecodeCapabilities.pNext = &h264Capabilities;
        } else if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
            videoDecodeCapabilities.pNext = &h265Capabilities;
        } else if (videoCodec == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            videoDecodeCapabilities.pNext = &av1Capabilities;
        } else {
            assert(!"Unsupported codec");
            return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
        }
        VkResult result = GetVideoCapabilities(vkDevCtx, videoProfile, &videoCapabilities);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: Input is not supported. GetVideoCapabilities() result: 0x%x\n", result);
        }
        return result;
    }

    static VkResult GetSupportedVideoFormats(const VulkanDeviceContext* vkDevCtx,
                                             const VkVideoCoreProfile& videoProfile,
                                             VkVideoDecodeCapabilityFlagsKHR capabilityFlags,
                                             VkFormat& pictureFormat,
                                             VkFormat& referencePicturesFormat)
    {
        VkResult result = VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;
        if ((capabilityFlags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0) {
            // NV, Intel
            VkFormat supportedDpbFormats[8];
            uint32_t formatCount = sizeof(supportedDpbFormats) / sizeof(supportedDpbFormats[0]);
            result = GetVideoFormats(vkDevCtx, videoProfile,
                                    (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR),
                                    formatCount, supportedDpbFormats);

            referencePicturesFormat = supportedDpbFormats[0];
            pictureFormat = supportedDpbFormats[0];

        } else if ((capabilityFlags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) != 0) {
            // AMD
            VkFormat supportedDpbFormats[8];
            VkFormat supportedOutFormats[8];
            uint32_t formatCount = sizeof(supportedDpbFormats) / sizeof(supportedDpbFormats[0]);
            result = GetVideoFormats(vkDevCtx, videoProfile,
                                    VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                                    formatCount, supportedDpbFormats);

            assert(result == VK_SUCCESS);

            result = GetVideoFormats(vkDevCtx, videoProfile,
                                    VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR,
                                    formatCount, supportedOutFormats);

            referencePicturesFormat = supportedDpbFormats[0];
            pictureFormat = supportedOutFormats[0];

        } else {
            fprintf(stderr, "\nERROR: Unsupported decode capability flags.");
            return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;
        }

        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: GetVideoFormats() result: 0x%x\n", result);
        }

        assert((referencePicturesFormat != VK_FORMAT_UNDEFINED) && (pictureFormat != VK_FORMAT_UNDEFINED));
        assert(referencePicturesFormat == pictureFormat);

        return result;
    }

    static VkResult GetVideoCapabilities(const VulkanDeviceContext* vkDevCtx,
                                         const VkVideoCoreProfile& videoProfile,
                                         VkVideoCapabilitiesKHR* pVideoCapabilities,
                                         bool dumpData = false)
    {
        assert(pVideoCapabilities->sType == VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
        VkVideoDecodeCapabilitiesKHR* pVideoDecodeCapabilities = (VkVideoDecodeCapabilitiesKHR*)pVideoCapabilities->pNext;
        assert(pVideoDecodeCapabilities->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
        VkVideoDecodeH264CapabilitiesKHR* pH264Capabilities = nullptr;
        VkVideoDecodeH265CapabilitiesKHR* pH265Capabilities = nullptr;
        VkVideoDecodeAV1CapabilitiesKHR* pAV1Capabilities = nullptr;

        if (videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
            assert(pVideoDecodeCapabilities->pNext);
            pH264Capabilities = (VkVideoDecodeH264CapabilitiesKHR*)pVideoDecodeCapabilities->pNext;
            assert(pH264Capabilities->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR);
        } else if (videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
            assert(pVideoDecodeCapabilities->pNext);
            pH265Capabilities = (VkVideoDecodeH265CapabilitiesKHR*)pVideoDecodeCapabilities->pNext;
            assert(pH265Capabilities->sType ==  VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR);
        } else if (videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
            assert(pVideoDecodeCapabilities->pNext);
            pAV1Capabilities = (VkVideoDecodeAV1CapabilitiesKHR*)pVideoDecodeCapabilities->pNext;
            assert(pAV1Capabilities->sType ==  VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR);
        } else {
            assert(!"Unsupported codec");
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
#if HEADLESS_AV1
        pVideoCapabilities->minCodedExtent.width  = 0x90;
        pVideoCapabilities->minCodedExtent.height = 0x90;
        pVideoCapabilities->maxCodedExtent.width  = 0x2000;
        pVideoCapabilities->maxCodedExtent.height = 0x2000;
        pVideoCapabilities->maxDpbSlots = 0x10;
        pVideoCapabilities->minBitstreamBufferOffsetAlignment = 0x100;
        pVideoCapabilities->minBitstreamBufferSizeAlignment   = 0x100;
        pVideoCapabilities->maxActiveReferencePictures = 0x10;
        pVideoCapabilities->flags = 2;
        VkVideoDecodeCapabilitiesKHR *videoDecodeCaps = (VkVideoDecodeCapabilitiesKHR*)pVideoCapabilities->pNext;
        videoDecodeCaps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
        VkResult result = VK_SUCCESS;
#else
        VkResult result = vkDevCtx->GetPhysicalDeviceVideoCapabilitiesKHR(vkDevCtx->getPhysicalDevice(),
                                                                            videoProfile.GetProfile(),
                                                                            pVideoCapabilities);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            return result;
        }
#endif

        if (dumpData) {
            std::cout << "\t\t\t" << ((videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) ? "h264" : "h265") << "decode capabilities: " << std::endl;

            if (pVideoCapabilities->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR) {
                std::cout << "\t\t\t" << "Use separate reference images" << std::endl;
            }

            std::cout << "\t\t\t" << "minBitstreamBufferOffsetAlignment: " << pVideoCapabilities->minBitstreamBufferOffsetAlignment << std::endl;
            std::cout << "\t\t\t" << "minBitstreamBufferSizeAlignment: " << pVideoCapabilities->minBitstreamBufferSizeAlignment << std::endl;
            std::cout << "\t\t\t" << "pictureAccessGranularity: " << pVideoCapabilities->pictureAccessGranularity.width << " x " << pVideoCapabilities->pictureAccessGranularity.height << std::endl;
            std::cout << "\t\t\t" << "minCodedExtent: " << pVideoCapabilities->minCodedExtent.width << " x " << pVideoCapabilities->minCodedExtent.height << std::endl;
            std::cout << "\t\t\t" << "maxCodedExtent: " << pVideoCapabilities->maxCodedExtent.width  << " x " << pVideoCapabilities->maxCodedExtent.height << std::endl;
            std::cout << "\t\t\t" << "maxDpbSlots: " << pVideoCapabilities->maxDpbSlots << std::endl;
            std::cout << "\t\t\t" << "maxActiveReferencePictures: " << pVideoCapabilities->maxActiveReferencePictures << std::endl;

            if (videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
                std::cout << "\t\t\t" << "maxLevelIdc: " << pH264Capabilities->maxLevelIdc << std::endl;
                std::cout << "\t\t\t" << "fieldOffsetGranularity: " << pH264Capabilities->fieldOffsetGranularity.x << " x " << pH264Capabilities->fieldOffsetGranularity.y << std::endl;;

                if (strncmp(pVideoCapabilities->stdHeaderVersion.extensionName,
                        VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
                            sizeof (pVideoCapabilities->stdHeaderVersion.extensionName) - 1U) ||
                    (pVideoCapabilities->stdHeaderVersion.specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION)) {
                    assert(!"Unsupported h.264 STD version");
                    return VK_ERROR_INCOMPATIBLE_DRIVER;
                }
            } else if (videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
                std::cout << "\t\t\t" << "maxLevelIdc: " << pH265Capabilities->maxLevelIdc << std::endl;
                if (strncmp(pVideoCapabilities->stdHeaderVersion.extensionName,
                        VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME,
                            sizeof (pVideoCapabilities->stdHeaderVersion.extensionName) - 1U) ||
                    (pVideoCapabilities->stdHeaderVersion.specVersion != VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION)) {
                    assert(!"Unsupported h.265 STD version");
                    return VK_ERROR_INCOMPATIBLE_DRIVER;
                }
            } else {
                assert(!"Unsupported codec");
            }
        }

        return result;
    }

    static VkResult GetVideoFormats(const VulkanDeviceContext* vkDevCtx,
                                    const VkVideoCoreProfile& videoProfile, VkImageUsageFlags imageUsage,
                                    uint32_t& formatCount, VkFormat* formats,
                                    bool dumpData = false)
    {
        for (uint32_t i = 0; i < formatCount; i++) {
            formats[i] = VK_FORMAT_UNDEFINED;
        }

        const VkVideoProfileListInfoKHR videoProfiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, nullptr, 1, videoProfile.GetProfile() };
        const VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, const_cast<VkVideoProfileListInfoKHR *>(&videoProfiles),
                                                                     imageUsage };

        uint32_t supportedFormatCount = 0;
#if HEADLESS_AV1
        supportedFormatCount = 1;
        VkResult result = VK_SUCCESS;
#else 
        VkResult result = vkDevCtx->GetPhysicalDeviceVideoFormatPropertiesKHR(vkDevCtx->getPhysicalDevice(), &videoFormatInfo, &supportedFormatCount, nullptr);
        assert(result == VK_SUCCESS);
        assert(supportedFormatCount);
#endif

        VkVideoFormatPropertiesKHR* pSupportedFormats = new VkVideoFormatPropertiesKHR[supportedFormatCount];
        memset(pSupportedFormats, 0x00, supportedFormatCount * sizeof(VkVideoFormatPropertiesKHR));
        for (uint32_t i = 0; i < supportedFormatCount; i++) {
            pSupportedFormats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        }

#if HEADLESS_AV1

#else
        result = vkDevCtx->GetPhysicalDeviceVideoFormatPropertiesKHR(vkDevCtx->getPhysicalDevice(), &videoFormatInfo, &supportedFormatCount, pSupportedFormats);
        assert(result == VK_SUCCESS);
#endif
        if (dumpData) {
            std::cout << "\t\t\t" << ((videoProfile.GetCodecType() == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) ? "h264" : "h265") << "decode formats: " << std::endl;
            for (uint32_t fmt = 0; fmt < supportedFormatCount; fmt++) {
                std::cout << "\t\t\t " << fmt << ": " << std::hex << pSupportedFormats[fmt].format << std::dec << std::endl;
            }
        }

        formatCount = std::min(supportedFormatCount, formatCount);

        for (uint32_t i = 0; i < formatCount; i++) {
            formats[i] = pSupportedFormats[i].format;
        }

        delete[] pSupportedFormats;

        return result;
    }

    static VkVideoCodecOperationFlagsKHR GetSupportedCodecs(const VulkanDeviceContext* vkDevCtx,
                                                            VkPhysicalDevice vkPhysicalDev,
                                                            int32_t* pVideoQueueFamily,
            VkQueueFlags queueFlagsRequired = ( VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR),
            VkVideoCodecOperationFlagsKHR videoCodeOperations =
                                              ( VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
                                                VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT | VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT))
    {
        std::vector<VkQueueFamilyProperties2> queues;
        std::vector<VkQueueFamilyVideoPropertiesKHR> videoQueues;
        std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> queryResultStatus;
        get(vkDevCtx, vkPhysicalDev, queues, videoQueues, queryResultStatus);

        for (uint32_t queueIndx = 0; queueIndx < queues.size(); queueIndx++) {
            const VkQueueFamilyProperties2 &q = queues[queueIndx];
            const VkQueueFamilyVideoPropertiesKHR &videoQueue = videoQueues[queueIndx];

            if (pVideoQueueFamily && (*pVideoQueueFamily >= 0) && (*pVideoQueueFamily != (int32_t)queueIndx)) {
                continue;
            }

            if ((q.queueFamilyProperties.queueFlags & queueFlagsRequired) &&
                (videoQueue.videoCodecOperations & videoCodeOperations)) {
                if (pVideoQueueFamily && (*pVideoQueueFamily < 0)) {
                    *pVideoQueueFamily = (int32_t)queueIndx;
                }
                // The video queues may or may not support queryResultStatus
                // assert(queryResultStatus[queueIndx].queryResultStatusSupport);
                return videoQueue.videoCodecOperations;
            }
        }

        return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }

    static VkVideoCodecOperationFlagsKHR GetSupportedCodecs(const VulkanDeviceContext* vkDevCtx,
                                                            uint32_t vkVideoDecodeQueueFamily)
    {
        int32_t videoDecodeQueueFamily = (int32_t)vkVideoDecodeQueueFamily;
        VkVideoCodecOperationFlagsKHR videoCodecs = GetSupportedCodecs(vkDevCtx, vkDevCtx->getPhysicalDevice(),
                                                                       &videoDecodeQueueFamily,
                                                                       VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                                                       (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                                                                        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR));

        assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_NONE_KHR);

        return videoCodecs;
    }

    static bool IsCodecTypeSupported(const VulkanDeviceContext* vkDevCtx, uint32_t vkVideoDecodeQueueFamily,
                                     VkVideoCodecOperationFlagBitsKHR videoCodec)
    {
        VkVideoCodecOperationFlagsKHR videoCodecs = GetSupportedCodecs(vkDevCtx, vkVideoDecodeQueueFamily);

        if (videoCodecs & videoCodec) {
            return true;
        }

        return false;
    }

    static VkResult GetDecodeH264Capabilities(const VulkanDeviceContext* vkDevCtx, uint32_t vkVideoDecodeQueueFamily,
                                              const VkVideoProfileInfoKHR& videoProfile,
                                              VkVideoCapabilitiesKHR &videoDecodeCapabilities)
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vkDevCtx->GetPhysicalDeviceVideoCapabilitiesKHR(vkDevCtx->getPhysicalDevice(),
                                                               &videoProfile,
                                                               &videoDecodeCapabilities);
    }

    static VkResult GetDecodeH265Capabilities(const VulkanDeviceContext* vkDevCtx, uint32_t vkVideoDecodeQueueFamily,
                                              const VkVideoProfileInfoKHR& videoProfile,
                                              VkVideoCapabilitiesKHR &videoDecodeCapabilities)
    {
        videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        return vkDevCtx->GetPhysicalDeviceVideoCapabilitiesKHR(vkDevCtx->getPhysicalDevice(),
                                                               &videoProfile,
                                                               &videoDecodeCapabilities);
    }

    static VkResult GetEncodeH264Capabilities(const VulkanDeviceContext* vkDevCtx, uint32_t vkVideoDecodeQueueFamily,
                                              const VkVideoProfileInfoKHR& videoProfile,
                                              VkVideoCapabilitiesKHR &videoEncodeCapabilities,
                                              VkVideoEncodeH264CapabilitiesEXT &encode264Capabilities)
    {
        encode264Capabilities.sType   = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT;
        videoEncodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
        videoEncodeCapabilities.pNext = &encode264Capabilities;
        return vkDevCtx->GetPhysicalDeviceVideoCapabilitiesKHR(vkDevCtx->getPhysicalDevice(),
                                                               &videoProfile,
                                                               &videoEncodeCapabilities);

    }

    static VkResult GetEncodeH264Capabilities(const VulkanDeviceContext* vkDevCtx,
                                              uint32_t vkVideoDecodeQueueFamily,
                                              const VkVideoCoreProfile* pProfile)
    {
        const bool isEncode = pProfile->IsEncodeCodecType();

        VkVideoEncodeH264CapabilitiesEXT encode264Capabilities = VkVideoEncodeH264CapabilitiesEXT();
        encode264Capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT;
        VkVideoCapabilitiesKHR videoDecodeCapabilities = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
                                                           isEncode ? &encode264Capabilities : NULL };
        return vkDevCtx->GetPhysicalDeviceVideoCapabilitiesKHR(vkDevCtx->getPhysicalDevice(),
                                                               pProfile->GetProfile(),
                                                               &videoDecodeCapabilities);
    }

};

#endif /* _VULKANVIDEOCAPABILITIES_H_ */
