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

    enum FilterType { YCBCRCOPY, YCBCRCLEAR, YCBCR2RGBA, RGBA2YCBCR };
    static constexpr uint32_t maxNumComputeDescr = 8;

    static constexpr VkImageAspectFlags validPlaneAspects = VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                            VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                            VK_IMAGE_ASPECT_PLANE_2_BIT;

    static constexpr VkImageAspectFlags validAspects = VK_IMAGE_ASPECT_COLOR_BIT | validPlaneAspects;

    static uint32_t GetPlaneIndex(VkImageAspectFlagBits planeAspect);

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType flterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           bool inputEnableMsbToLsbShift,
                           bool outputEnableLsbToMsbShift,
                           const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants,
                           const VkSamplerCreateInfo* pSamplerCreateInfo,
                           VkSharedBaseObj<VulkanFilter>& vulkanFilter);

    VulkanFilterYuvCompute(const VulkanDeviceContext* vkDevCtx,
                           uint32_t queueFamilyIndex,
                           uint32_t queueIndex,
                           FilterType filterType,
                           uint32_t maxNumFrames,
                           VkFormat inputFormat,
                           VkFormat outputFormat,
                           bool inputEnableMsbToLsbShift,
                           bool outputEnableLsbToMsbShift,
                           const YcbcrPrimariesConstants* pYcbcrPrimariesConstants)
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
        , m_inputEnableMsbToLsbShift(inputEnableMsbToLsbShift)
        , m_outputEnableLsbToMsbShift(outputEnableLsbToMsbShift)
        , m_enableRowAndColumnReplication(true)
        , m_inputIsBuffer(false)
        , m_outputIsBuffer(false)
    {
    }

    VkResult Init(const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                  const VkSamplerCreateInfo* pSamplerCreateInfo);

    virtual ~VulkanFilterYuvCompute() {
        assert(m_vkDevCtx != nullptr);
    }

    uint32_t UpdateBufferDescriptorSets(const VkBuffer*            vkBuffers,
                                        uint32_t                   numVkBuffers,
                                        const VkSubresourceLayout* vkBufferSubresourceLayout,
                                        uint32_t                   numPlanes,
                                        VkImageAspectFlags         validImageAspects,
                                        uint32_t&                  descrIndex,
                                        uint32_t&                  baseBinding,
                                        VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                        VkDescriptorBufferInfo bufferDescriptors[maxNumComputeDescr],
                                        std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                        const uint32_t maxDescriptors = maxNumComputeDescr);

    uint32_t  UpdateImageDescriptorSets(const VkImageResourceView* inputImageView,
                                        VkImageAspectFlags         validImageAspects,
                                        VkSampler                  convSampler,
                                        VkImageLayout              imageLayout,
                                        uint32_t&                  descrIndex,
                                        uint32_t&                  baseBinding,
                                        VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                        VkDescriptorImageInfo      imageDescriptors[maxNumComputeDescr],
                                        std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                        const uint32_t maxDescriptors = maxNumComputeDescr);

    // Image input -> Image output
    virtual VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                         const VkImageResourceView* inputImageView,
                                         const VkVideoPictureResourceInfoKHR * inputImageResourceInfo,
                                         const VkImageResourceView* outputImageView,
                                         const VkVideoPictureResourceInfoKHR * outputImageResourceInfo,
                                         uint32_t bufferIdx) override;
    // Buffer input -> Image output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                const VkBuffer*            inBuffers,     // with size numInBuffers
                                uint32_t                   numInBuffers,
                                const VkFormat*            inBufferFormats, // with size inBufferNumPlanes
                                const VkSubresourceLayout* inBufferSubresourceLayouts, // with size inBufferNumPlanes
                                uint32_t                   inBufferNumPlanes,
                                const VkImageResourceView* outImageView,
                                const VkVideoPictureResourceInfoKHR* outImageResourceInfo,
                                const VkBufferImageCopy*   pBufferImageCopy,
                                uint32_t bufferIdx);

    // Image input -> Buffer output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                const VkImageResourceView* inImageView,
                                const VkVideoPictureResourceInfoKHR* inImageResourceInfo,
                                const VkBuffer*            outBuffers,        // with size numOutBuffers
                                uint32_t                   numOutBuffers,
                                const VkFormat*            inBufferFormats,   // with size outBufferNumPlanes
                                const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                uint32_t                   outBufferNumPlanes,
                                const VkBufferImageCopy*   pBufferImageCopy,
                                uint32_t bufferIdx);

    // Buffer input -> Buffer output
    VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                 const VkBuffer*            inBuffers,       // with size numInBuffers
                                 uint32_t                   numInBuffers,
                                 const VkFormat*            inBufferFormats, // with size inBufferNumPlanes
                                 const VkSubresourceLayout* inBufferSubresourceLayouts, // with size inBufferNumPlanes
                                 uint32_t                   inBufferNumPlanes,
                                 const VkExtent3D&          inBufferExtent,
                                 const VkBuffer*            outBuffers,        // with size numOutBuffers
                                 uint32_t                   numOutBuffers,
                                 const VkFormat*            outBufferFormats,   // with size outBufferNumPlanes
                                 const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                 uint32_t                   outBufferNumPlanes,
                                 const VkExtent3D&          outBufferExtent,
                                 uint32_t bufferIdx);

private:
    VkResult InitDescriptorSetLayout(uint32_t maxNumFrames);

    /**
     * @brief Generates GLSL image descriptor bindings for shader input/output
     *
     * Creates appropriate GLSL image binding declarations based on the input/output format.
     * Handles different YUV formats like single-plane (RGBA), 2-plane (NV12/NV21), and 3-plane (I420, etc).
     *
     * @param computeShader Output stringstream for shader code
     * @param imageAspects Output parameter to store the image aspect flags used
     * @param imageName Base image variable name
     * @param imageFormat Vulkan format of the image
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param imageArray Whether to use image2DArray or image2D
     * @return The next available binding number after all descriptors are created
     */
    uint32_t ShaderGenerateImagePlaneDescriptors(std::stringstream& computeShader,
                                                 VkImageAspectFlags& imageAspects,
                                                 const char *imageName,
                                                 VkFormat    imageFormat,
                                                 bool isInput,
                                                 uint32_t startBinding = 0,
                                                 uint32_t set = 0,
                                                 bool imageArray = true);

    /**
     * @brief Generates GLSL buffer descriptor bindings for shader input/output
     *
     * Creates appropriate GLSL buffer binding declarations based on the input/output format.
     * Handles different YUV buffer layouts matching single-plane, 2-plane, or 3-plane formats.
     *
     * @param shaderStr Output stringstream for shader code
     * @param imageAspects Output parameter to store the image aspect flags used
     * @param bufferName Base buffer variable name
     * @param bufferFormat Vulkan format of the buffer data
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param bufferType The Vulkan descriptor type to use for the buffer
     * @return The next available binding number after all descriptors are created
     */
    uint32_t ShaderGenerateBufferPlaneDescriptors(std::stringstream& shaderStr,
                                                  VkImageAspectFlags& imageAspects,
                                                  const char *bufferName,
                                                  VkFormat    bufferFormat,
                                                  bool isInput,
                                                  uint32_t startBinding = 0,
                                                  uint32_t set = 0,
                                                  VkDescriptorType bufferType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    /**
     * @brief Unified descriptor generation for either buffer or image resources
     *
     * Delegates to either ShaderGenerateImagePlaneDescriptors or ShaderGenerateBufferPlaneDescriptors
     * based on the resource type (image or buffer) needed for input/output.
     *
     * @param shaderStr Output stringstream for shader code
     * @param isInput Whether this is an input or output resource
     * @param startBinding Starting binding number in the descriptor set
     * @param set Descriptor set number
     * @param imageArray Whether to use image2DArray or image2D (for image resources)
     * @param bufferType The Vulkan descriptor type to use for buffer resources
     * @return The next available binding number after all descriptors are created
     */
    uint32_t ShaderGeneratePlaneDescriptors(std::stringstream& shaderStr,
                                            bool isInput,
                                            uint32_t startBinding,
                                            uint32_t set,
                                            bool imageArray,
                                            VkDescriptorType bufferType);

    /**
     * @brief Initializes GLSL shader for YCbCr copy operation
     *
     * Generates a compute shader that copies YCbCr data from input to output
     * without any color space conversion, preserving the format.
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCRCOPY(std::string& computeShader);

    /**
     * @brief Initializes GLSL shader for YCbCr clear operation
     *
     * Generates a compute shader that clears/fills YCbCr data in the output
     * resource with constant values.
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCRCLEAR(std::string& computeShader);

    /**
     * @brief Initializes GLSL shader for YCbCr to RGBA conversion
     *
     * Generates a compute shader that converts YCbCr input to RGBA output
     * using the appropriate color space conversion matrix.
     *
     * @param computeShader Output string for the complete GLSL shader code
     * @return Size of the generated shader code in bytes
     */
    size_t InitYCBCR2RGBA(std::string& computeShader);

private:
    const FilterType                         m_filterType;
    VkFormat                                 m_inputFormat;
    VkFormat                                 m_outputFormat;
    uint32_t                                 m_workgroupSizeX; // usually 16
    uint32_t                                 m_workgroupSizeY; // usually 16
    uint32_t                                 m_maxNumFrames;
    const YcbcrPrimariesConstants            m_ycbcrPrimariesConstants;
    VulkanSamplerYcbcrConversion             m_samplerYcbcrConversion;
    VulkanDescriptorSetLayout                m_descriptorSetLayout;
    VulkanComputePipeline                    m_computePipeline;
    VkImageAspectFlags                       m_inputImageAspects;
    VkImageAspectFlags                       m_outputImageAspects;
    uint32_t                                 m_inputEnableMsbToLsbShift : 1;
    uint32_t                                 m_outputEnableLsbToMsbShift : 1;
    uint32_t                                 m_enableRowAndColumnReplication : 1;
    uint32_t                                 m_inputIsBuffer : 1;
    uint32_t                                 m_outputIsBuffer : 1;

    struct PushConstants {
        uint32_t srcLayer;        // src image layer to use
        uint32_t dstLayer;        // dst image layer to use
        uint32_t inputWidth;      // input image or buffer width
        uint32_t inputHeight;     // input image or buffer height
        uint32_t outputWidth;     // output image or buffer width
        uint32_t outputHeight;    // output image or buffer height
        uint32_t inYOffset;       // input buffer Y plane offset
        uint32_t inCbOffset;      // input buffer Cb plane offset
        uint32_t inCrOffset;      // input buffer Cr plane offset
        uint32_t inYPitch;        // input buffer Y plane pitch
        uint32_t inCbPitch;       // input buffer Cb plane pitch
        uint32_t inCrPitch;       // input buffer Cr plane pitch
        uint32_t outYOffset;      // output buffer Y plane offset
        uint32_t outCbOffset;     // output buffer Cb plane offset
        uint32_t outCrOffset;     // output buffer Cr plane offset
        uint32_t outYPitch;       // output buffer Y plane pitch
        uint32_t outCbPitch;      // output buffer Cb plane pitch
        uint32_t outCrPitch;      // output buffer Cr plane pitch
    };
};

#endif /* _VULKANFILTERYUVCOMPUTE_H_ */
