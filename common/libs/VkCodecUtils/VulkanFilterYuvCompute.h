/*
* Copyright 2023 NVIDIA Corporation.
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


#ifndef _VULKANFILTERYUVCOMPUTE_H_
#define _VULKANFILTERYUVCOMPUTE_H_

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanCommandBuffersSet.h"
#include "VkCodecUtils/VulkanSemaphoreSet.h"
#include "VkCodecUtils/VulkanFenceSet.h"
#include "VkCodecUtils/VulkanDescriptorSetLayout.h"
#include "VkCodecUtils/VulkanComputePipeline.h"
#include "VkCodecUtils/VulkanFilter.h"
#include "nvidia_utils/vulkan/ycbcr_utils.h"

class VulkanFilterYuvCompute : public VulkanFilter
{
public:

    enum FilterType { YCBCRCOPY, YCBCRCLEAR, YCBCR2RGBA, RGBA2YCBCR, RESIZE };

    struct Rectangle {
        int width = 0;
        int height = 0;
    };

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType flterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants,
                           const VkSamplerCreateInfo* pSamplerCreateInfo,
                           VkSharedBaseObj<VulkanFilter>& vulkanFilter
#ifdef _TRANSCODING
                            , const std::vector<Rectangle>& resizedResolutions = {}
#endif
                            );

    VulkanFilterYuvCompute(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType filterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants
#ifdef _TRANSCODING
                           , const std::vector<Rectangle>& resizedResolutions = {}
#endif
                            )
        : VulkanFilter(vkDevCtx, queueFamilyIndex, queueIndex)
        , m_filterType(filterType)
        , m_inputFormat(inputFormat)
        , m_outputFormat(outputFormat)
        , m_workgroupSizeX(16)
        , m_workgroupSizeY(16)
        , m_maxNumFrames(maxNumFrames)
        , m_ycbcrPrimariesConstants (pYcbcrPrimariesConstants ?
                                        *pYcbcrPrimariesConstants :
                                        YcbcrPrimariesConstants{0.0, 0.0})
        , m_inputImageAspects(  VK_IMAGE_ASPECT_COLOR_BIT |
                                VK_IMAGE_ASPECT_PLANE_0_BIT |
                                VK_IMAGE_ASPECT_PLANE_1_BIT |
                                VK_IMAGE_ASPECT_PLANE_2_BIT)
        , m_outputImageAspects( VK_IMAGE_ASPECT_COLOR_BIT |
                                VK_IMAGE_ASPECT_PLANE_0_BIT |
                                VK_IMAGE_ASPECT_PLANE_1_BIT |
                                VK_IMAGE_ASPECT_PLANE_2_BIT)
#ifdef _TRANSCODING
        , m_resizedResolutions( resizedResolutions )
#endif
    {
    }

    VkResult Init(const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                  const VkSamplerCreateInfo* pSamplerCreateInfo);

    virtual ~VulkanFilterYuvCompute() {
        assert(m_vkDevCtx != nullptr);
    }

    virtual VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                         const VkImageResourceView* inputImageView,
                                         const VkVideoPictureResourceInfoKHR * inputImageResourceInfo,
                                         const VkImageResourceView* outputImageView,
                                         const VkVideoPictureResourceInfoKHR * outputImageResourceInfo,
                                         uint32_t bufferIdx)
    {

        assert(cmdBuf != VK_NULL_HANDLE);

        m_vkDevCtx->CmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline.getPipeline());

        VkDescriptorSetLayoutCreateFlags layoutMode = m_descriptorSetLayout.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode();

        switch (layoutMode) {
            case VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR:
            case VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT:
            {

                const uint32_t maxNumComputeDescr = 8;
                VkDescriptorImageInfo imageDescriptors[8]{};
                std::array<VkWriteDescriptorSet, maxNumComputeDescr> writeDescriptorSets{};

                // Images
                uint32_t set = 0;
                uint32_t descrIndex = 0;
                uint32_t dstBinding = 0;
                // RGBA color converted by an YCbCr sample
                if (m_inputImageAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = (m_samplerYcbcrConversion.GetSampler() != VK_NULL_HANDLE) ?
                                                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                                                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

                    imageDescriptors[descrIndex].sampler = m_samplerYcbcrConversion.GetSampler();
                    imageDescriptors[descrIndex].imageView = inputImageView->GetImageView();
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // RGBA or Sampled YCbCr
                    descrIndex++;
                }
                dstBinding++;

                uint32_t planeNum = 0;
                // y plane - G -> R8
                if ((m_inputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < inputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = inputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // Y (0) plane
                    descrIndex++;
                }
                dstBinding++;

                // CbCr plane - BR -> R8B8
                if ((m_inputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < inputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = inputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // CbCr (1) plane
                    descrIndex++;
                }
                dstBinding++;

                // Cr plane - R -> R8
                if ((m_inputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < inputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = inputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // CbCr (1) plane
                    descrIndex++;
                }
                dstBinding++;

                // Out RGBA or single planar YCbCr image
                if (m_outputImageAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = outputImageView->GetImageView();
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex];
                    descrIndex++;
                }
                dstBinding++;

                planeNum = 0;
                // y plane out - G -> R8
                if ((m_outputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < outputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = outputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex];
                    descrIndex++;
                }
                dstBinding++;

                // CbCr plane out - BR -> R8B8
                if ((m_outputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < outputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = outputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex];
                    descrIndex++;
                }
                dstBinding++;

                // Cr plane out - R -> R8
                if ((m_outputImageAspects & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) &&
                        (planeNum < outputImageView->GetNumberOfPlanes())) {
                    writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                    writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                    writeDescriptorSets[descrIndex].descriptorCount = 1;
                    writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                    imageDescriptors[descrIndex].imageView = outputImageView->GetPlaneImageView(planeNum++);
                    assert(imageDescriptors[descrIndex].imageView);
                    imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex];
                    descrIndex++;
                }
                dstBinding++;

                assert(descrIndex <= maxNumComputeDescr);
                assert(descrIndex >= 2);

                if (layoutMode ==  VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
                    m_vkDevCtx->CmdPushDescriptorSetKHR(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                        m_descriptorSetLayout.GetPipelineLayout(),
                                                        set, descrIndex, writeDescriptorSets.data());
                } else {

                    VkDeviceOrHostAddressConstKHR imageDescriptorBufferDeviceAddress =
                          m_descriptorSetLayout.UpdateDescriptorBuffer(bufferIdx,
                                                                       set,
                                                                       descrIndex,
                                                                       writeDescriptorSets.data());


                    // Descriptor buffer bindings
                    // Set 0 = Image
                    VkDescriptorBufferBindingInfoEXT bindingInfo{};
                    bindingInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                    bindingInfo.pNext = nullptr;
                    bindingInfo.address = imageDescriptorBufferDeviceAddress.deviceAddress;
                    bindingInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                                        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                    m_vkDevCtx->CmdBindDescriptorBuffersEXT(cmdBuf, 1, &bindingInfo);

                    // Image (set 0)
                    uint32_t bufferIndexImage = 0;
                    VkDeviceSize bufferOffset = 0;
                    m_vkDevCtx->CmdSetDescriptorBufferOffsetsEXT(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                               m_descriptorSetLayout.GetPipelineLayout(),
                                                               set, 1, &bufferIndexImage, &bufferOffset);
                }
            }
            break;

            default:
            m_vkDevCtx->CmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                              m_descriptorSetLayout.GetPipelineLayout(),
                                              0, 1, m_descriptorSetLayout.GetDescriptorSet(), 0, 0);
        }

        struct PushConstants {
            uint32_t srcLayer;
            uint32_t dstLayer;
#if(_TRANSCODING)
            uint32_t width;
            uint32_t height;
            float scalingFactorX;
            float scalingFactorY;
#endif //_TRANSCODING
        };

#if(_TRANSCODING)
        int numResizes = m_resizedResolutions.size();

        for (int i = 0; i < numResizes; i++)
        {
            float resizedWidth =  m_resizedResolutions[i].width;
            float resizedHeight =  m_resizedResolutions[i].height;
#endif //_TRANSCODING
            const VkImageCreateInfo& imageCreateInfo = outputImageView->GetImageResource()->GetImageCreateInfo();
            uint32_t  width  = imageCreateInfo.extent.width;
            uint32_t  height = imageCreateInfo.extent.height;

            PushConstants pushConstants = {
                    inputImageResourceInfo  ? inputImageResourceInfo->baseArrayLayer : 0, // Set the source layer index
#if(!_TRANSCODING)
                    outputImageResourceInfo ? outputImageResourceInfo->baseArrayLayer : 0, // Set the destination layer index
#else
                    outputImageResourceInfo ? outputImageResourceInfo->baseArrayLayer + i : i, // Set the destination layer index
                    width,
                    height,
                    width / resizedWidth,
                    height / resizedHeight
#endif //_TRANSCODING
            };

            m_vkDevCtx->CmdPushConstants(cmdBuf,
                                            m_descriptorSetLayout.GetPipelineLayout(),
                                            VK_SHADER_STAGE_COMPUTE_BIT,
                                            0, // offset
                                            sizeof(PushConstants),
                                            &pushConstants);

#if(_TRANSCODING)
            const uint32_t yuv_scale_x = 2; // yuv420 & yuv 422 only
            const uint32_t yuv_scale_y = 2; // yuv420 only
            int numPixelsPerInvocationX = yuv_scale_x * m_workgroupSizeX;
            int numPixelsPerInvocationY = yuv_scale_y * m_workgroupSizeY;
            uint32_t numTotalInvocationsRoundedUpX = (resizedWidth + numPixelsPerInvocationX - 1) / numPixelsPerInvocationX;
            uint32_t numTotalInvocationsRoundedUpY = (resizedHeight + numPixelsPerInvocationY  - 1) / numPixelsPerInvocationY;

            m_vkDevCtx->CmdDispatch(cmdBuf, numTotalInvocationsRoundedUpX, numTotalInvocationsRoundedUpY, 1);
        }
#else
        m_vkDevCtx->CmdDispatch(cmdBuf, width / m_workgroupSizeX, height / m_workgroupSizeY, 1);
#endif //_TRANSCODING

        return VK_SUCCESS;
    }

private:
    VkResult InitDescriptorSetLayout(uint32_t maxNumFrames);
    size_t InitYCBCRCOPY(std::string& computeShader);
    size_t InitYCBCRCLEAR(std::string& computeShader);
    size_t InitYCBCR2RGBA(std::string& computeShader);
    size_t InitResize(std::string& computeShader);

private:
    const FilterType                         m_filterType;
    VkFormat                                 m_inputFormat;
    VkFormat                                 m_outputFormat;
    uint32_t                                 m_workgroupSizeX; // usually 16
    uint32_t                                 m_workgroupSizeY; // usually 16
    uint32_t                                 m_maxNumFrames;
    const YcbcrPrimariesConstants            m_ycbcrPrimariesConstants;
    VulkanSamplerYcbcrConversion             m_samplerYcbcrConversion;
    VulkanSamplerResize                      m_samplerResize;
    VulkanDescriptorSetLayout                m_descriptorSetLayout;
    VulkanComputePipeline                    m_computePipeline;
    VkImageAspectFlags                       m_inputImageAspects;
    VkImageAspectFlags                       m_outputImageAspects;
#ifdef _TRANSCODING
    std::vector<Rectangle>                   m_resizedResolutions;
#endif

};

#endif /* _VULKANFILTERYUVCOMPUTE_H_ */
