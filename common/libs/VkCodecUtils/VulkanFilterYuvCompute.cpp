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


#include "VulkanFilterYuvCompute.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

static bool dumpShaders = false;

VkResult VulkanFilterYuvCompute::Create(const VulkanDeviceContext* vkDevCtx,
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
                                        , const std::vector<Rectangle>& resizedResolutions
#endif
)
{

    VkSharedBaseObj<VulkanFilterYuvCompute> yCbCrVulkanFilter(new VulkanFilterYuvCompute(vkDevCtx,
                                                                                         queueFamilyIndex,
                                                                                         queueIndex,
                                                                                         flterType,
                                                                                         maxNumFrames,
                                                                                         inputFormat,
                                                                                         outputFormat,
                                                                                         pYcbcrPrimariesConstants
#ifdef _TRANSCODING
                                                                                         , resizedResolutions
#endif
                                                                                         ));

    if (!yCbCrVulkanFilter) {
       return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult result = yCbCrVulkanFilter->Init(pYcbcrConversionCreateInfo, pSamplerCreateInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    vulkanFilter = yCbCrVulkanFilter;
    return VK_SUCCESS;
}

VkResult VulkanFilterYuvCompute::Init(const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                                      const VkSamplerCreateInfo* pSamplerCreateInfo)
{
    VkResult result = Configure( m_vkDevCtx,
                                 m_maxNumFrames, // numPoolNodes
                                 m_vkDevCtx->GetComputeQueueFamilyIdx(), // queueFamilyIndex
                                 false,    // createQueryPool - not needed for the compute filter
                                 nullptr,  // pVideoProfile   - not needed for the compute filter
                                 true,     // createSemaphores
                                 true      // createFences
                                );

    if (pYcbcrConversionCreateInfo) {
         result = m_samplerYcbcrConversion.CreateVulkanSampler(m_vkDevCtx,
                                                               pSamplerCreateInfo,
                                                               pYcbcrConversionCreateInfo);
         if (result != VK_SUCCESS) {
             assert(!"ERROR: samplerYcbcrConversion!");
             return result;
         }
    }

#if (_TRANSCODING)
   if (m_filterType == RESIZE)
   {
        result = m_samplerResize.CreateVulkanSampler(m_vkDevCtx);
        if (result != VK_SUCCESS) {
            assert(!"ERROR: samplerResizeCreate!");
            return result;
        }
   }
#endif

    assert(m_queue != VK_NULL_HANDLE);

    result = InitDescriptorSetLayout(m_maxNumFrames);
    if (result != VK_SUCCESS) {
        assert(!"ERROR: InitDescriptorSetLayout!");
        return result;
    }

    std::string computeShader;
    size_t computeShaderSize = 0;
    switch (m_filterType) {
     case YCBCRCOPY:
         computeShaderSize = InitYCBCRCOPY(computeShader);
         break;
     case YCBCRCLEAR:
         computeShaderSize = InitYCBCRCLEAR(computeShader);
         break;
     case YCBCR2RGBA:
         computeShaderSize = InitYCBCR2RGBA(computeShader);
         break;
     case RGBA2YCBCR:
         assert(!"TODO RGBA2YCBCR");
         break;
    case RESIZE:
         computeShaderSize = InitResize(computeShader);
         break;
     default:
         assert(!"Invalid filter type");
         break;
    }

    return m_computePipeline.CreatePipeline(m_vkDevCtx, m_vulkanShaderCompiler,
                                            computeShader.c_str(), computeShaderSize,
                                            "main",
                                            m_workgroupSizeX, m_workgroupSizeY,
                                            &m_descriptorSetLayout);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VkResult VulkanFilterYuvCompute::InitDescriptorSetLayout(uint32_t maxNumFrames)
{

    VkSampler ccSampler = m_samplerYcbcrConversion.GetSampler();
    VkSampler textureSampler = m_samplerResize.GetSampler();
    assert(ccSampler != VK_NULL_HANDLE);
    VkDescriptorType type = (ccSampler != VK_NULL_HANDLE) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkSampler* pImmutableSamplersResize = &textureSampler;

    const std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
        //                        binding,  descriptorType,          descriptorCount, stageFlags, pImmutableSamplers;
        // Binding 0: Input image (read-only) RGBA or RGBA YCbCr sampler sampled
        VkDescriptorSetLayoutBinding{ 0, type,                             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // Binding 1: Input image (read-only) Y plane of YCbCr Image
        VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, pImmutableSamplersResize},
        // Binding 2: Input image (read-only) Cb or CbCr plane
        VkDescriptorSetLayoutBinding{ 2, type, 1, VK_SHADER_STAGE_COMPUTE_BIT,  pImmutableSamplersResize},
        // Binding 3: Input image (read-only) Cr plane
        VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},

        // Binding 4: Output image (write) RGBA or YCbCr single-plane image
        VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // Binding 5: Output image (write) Y plane of YCbCr Image
        VkDescriptorSetLayoutBinding{ 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // Binding 6: Output image (write) CbCr plane of 2-plane or Cb of 3-plane YCbCr Image
        VkDescriptorSetLayoutBinding{ 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        // Binding 7: Output image (write) Cr plane of 3-pane YCbCr Image
        VkDescriptorSetLayoutBinding{ 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},

        // Binding 8: uniform buffer for input parameters.
        VkDescriptorSetLayoutBinding{ 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; // Stage the push constant is for
    pushConstantRange.offset = 0;
    pushConstantRange.size = 6 * sizeof(uint32_t); // Size of the push constant - source and destination image layers + 2 * ivec2

    return m_descriptorSetLayout.CreateDescriptorSet(m_vkDevCtx,
                                                     setLayoutBindings,
                                                     VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                     1, &pushConstantRange,
                                                     &m_samplerYcbcrConversion,
                                                     maxNumFrames,
                                                     false);
}

static YcbcrBtStandard GetYcbcrPrimariesConstantsId(VkSamplerYcbcrModelConversion modelConversion)
{
    switch (modelConversion) {
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709:
        return YcbcrBtStandardBt709;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:
        return YcbcrBtStandardBt601Ebu;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
        return YcbcrBtStandardBt709;
    default:
        ;// assert(0);
    }

    return YcbcrBtStandardUnknown;
}

static void GenHeaderAndPushConst(std::stringstream& shaderStr)
{
    shaderStr << "#version 450\n"
                 "layout(push_constant) uniform PushConstants {\n"
                 "    uint srcImageLayer;  // Source image layer index\n"
                 "    uint dstImageLayer;  // Destination image layer index\n"
                 "    ivec2 inputSize;     // Original input image size (width, height)\n"
                 "    ivec2 outputSize;    // Output image size (width, height, with padding)\n"
                 "} pushConstants;\n"
                 "\n"
                 "layout (local_size_x = 16, local_size_y = 16) in;\n"
                 "\n";
}

static void GenImageIoBindingLayout(std::stringstream& shaderStr,
                                    const char *imageName,
                                    const char *imageSubName,
                                    const char *imageFormat,
                                    bool isInput,
                                    uint32_t binding,
                                    uint32_t set,
                                    bool imageArray) {

    shaderStr << "layout (set = " << set << ", binding = " << binding << ", " << imageFormat << ") uniform"
              << (isInput ? " readonly" : " writeonly")
              << (imageArray ? " image2DArray " : " image2D ")
              << imageName << imageSubName
              << ";\n";

}

static void GenHandleImagePosition(std::stringstream& shaderStr)
{
    shaderStr <<
    "    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n"
    "    // Check for out-of-bounds writes\n"
    "    if ((pos.x >= pushConstants.outputSize.x) || (pos.y >= pushConstants.outputSize.y)) {\n"
    "        return;\n"
    "    }\n"
    "\n";
}

static void GenHandleSourcePositionWithReplicate(std::stringstream& shaderStr, bool enableReplicate)
{
    if (enableReplicate) {
        shaderStr <<
        "    ivec2 srcPos = min(pos, pushConstants.inputSize );\n"
        "\n";
    } else {
        shaderStr <<
        "    ivec2 srcPos = pos;\n"
        "\n";
    }
}

void VulkanFilterYuvCompute::ShaderGeneratePlaneDescriptors(std::stringstream& shaderStr,
                                                            VkImageAspectFlags& imageAspects,
                                                            const char *imageName,
                                                            VkFormat    imageFormat,
                                                            bool isInput,
                                                            uint32_t startBinding,
                                                            uint32_t set,
                                                            bool imageArray)
{
    // Image binding goes in this pattern:
    // offset 0: RGBA image
    // offset 1: multi-planar image plane Y
    // offset 2: 2-planar image plane CbCr or 3-planar image plane Cb
    // offset 3: 3-planar image plane Cr
    const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(imageFormat);
    if (inputMpInfo) {

        GenImageIoBindingLayout(shaderStr, imageName, "Y",
                                vkFormatLookUp(inputMpInfo->vkPlaneFormat[0])->name,
                                isInput,
                                ++startBinding,
                                set,
                                imageArray);

        if (inputMpInfo->planesLayout.numberOfExtraPlanes == 1) {

            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

            GenImageIoBindingLayout(shaderStr, imageName, "CbCr",
                                    vkFormatLookUp(inputMpInfo->vkPlaneFormat[1])->name,
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);

        } else if (inputMpInfo->planesLayout.numberOfExtraPlanes == 2) {

            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;

            GenImageIoBindingLayout(shaderStr, imageName, "Cb",
                                    vkFormatLookUp(inputMpInfo->vkPlaneFormat[1])->name,
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);

            GenImageIoBindingLayout(shaderStr, imageName, "Cr",
                                    vkFormatLookUp(inputMpInfo->vkPlaneFormat[2])->name,
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);
        }
    } else {

        imageAspects = VK_IMAGE_ASPECT_COLOR_BIT;

        GenImageIoBindingLayout(shaderStr, imageName, "RGB",
                                vkFormatLookUp(imageFormat)->name,
                                isInput,
                                startBinding,
                                set,
                                imageArray);
    }
}

size_t VulkanFilterYuvCompute::InitYCBCR2RGBA(std::string& computeShader)
{
    // The compute filter uses two or three input images as separate planes
    // Y (R) binding = 1
    // 2-planar: CbCr (RG) binding = 2
    // OR
    // 3-planar: Cb (R) binding = 2
    // 3-planar: Cr (R) binding = 3

    // Create compute pipeline
    std::stringstream shaderStr;
    GenHeaderAndPushConst(shaderStr);
    // Input image
    shaderStr << " // The input YCbCr image binding\n";
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   m_inputImageAspects,
                                   "inputImage",
                                   m_inputFormat,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   true  // imageArray
                                   );

        // Output image
        shaderStr << " // The output RGBA image binding\n";
        ShaderGeneratePlaneDescriptors(shaderStr,
                                       m_outputImageAspects,
                                       "outputImage",
                                       m_outputFormat,
                                       false, // isInput
                                       4,     // startBinding
                                       0,     // set
                                       true   // imageArray
                                       );

        shaderStr << "\n"
                     " // TODO: normalize only narrow\n"
                     "float normalizeY(float Y) {\n"
                      "    // return (Y - (16.0 / 255.0)) * (255.0 / (235.0 - 16.0));\n"
                      "    return (Y - 0.0627451) * 1.164383562;\n"
                     "}\n"
                     "\n"
                     "vec2 shiftCbCr(vec2 CbCr) {\n"
                     "    return CbCr - 0.5;\n"
                     "}\n"
                     "\n"
                     "vec3 shiftCbCr(vec3 ycbcr) {\n"
                     "    const vec3 shiftCbCr  = vec3(0.0, -0.5, -0.5);\n"
                     "    return ycbcr + shiftCbCr;\n"
                     "}\n"
                     "\n"
                     " // TODO: normalize only narrow\n"
                     "vec2 normalizeCbCr(vec2 CbCr) {\n"
                     "    // return (CbCr - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0);\n"
                     "    return (CbCr - 0.0627451) * 1.138392857;\n"
                     "}\n"
                     "\n";

    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo = m_samplerYcbcrConversion.GetSamplerYcbcrConversionCreateInfo();
    const VkMpFormatInfo * mpInfo = YcbcrVkFormatInfo(samplerYcbcrConversionCreateInfo.format);
    const unsigned int bpp = (8 + mpInfo->planesLayout.bpp * 2);

    const YcbcrBtStandard btStandard = GetYcbcrPrimariesConstantsId(samplerYcbcrConversionCreateInfo.ycbcrModel);
    const YcbcrPrimariesConstants primariesConstants = GetYcbcrPrimariesConstants(btStandard);
    const YcbcrRangeConstants rangeConstants = GetYcbcrRangeConstants(YcbcrLevelsDigital);
    const YcbcrBtMatrix yCbCrMatrix(primariesConstants.kb,
                                    primariesConstants.kr,
                                    rangeConstants.cbMax,
                                    rangeConstants.crMax);


    shaderStr <<
        "vec3 convertYCbCrToRgb(vec3 yuv) {\n"
        "    vec3 rgb;\n";
    yCbCrMatrix.ConvertYCbCrToRgbString(shaderStr);
    shaderStr <<
        "    return rgb;\n"
        "}\n"
        "\n";


    YcbcrNormalizeColorRange yCbCrNormalizeColorRange(bpp,
            (samplerYcbcrConversionCreateInfo.ycbcrModel == VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY) ?
                    YCBCR_COLOR_RANGE_NATURAL : (YCBCR_COLOR_RANGE)samplerYcbcrConversionCreateInfo.ycbcrRange);
    shaderStr <<
        "vec3 normalizeYCbCr(vec3 yuv) {\n"
        "    vec3 yuvNorm;\n";
    yCbCrNormalizeColorRange.NormalizeYCbCrString(shaderStr);
    shaderStr <<
        "    return yuvNorm;\n"
        "}\n"
        "\n";

    shaderStr <<
        "void main()\n"
        "{\n";
    GenHandleImagePosition(shaderStr);
    GenHandleSourcePositionWithReplicate(shaderStr, m_enableRowAndColumnReplication);
    shaderStr <<
        "    // Fetch from the texture.\n"
        "    float Y = imageLoad(inputImageY, ivec3(srcPos, pushConstants.srcImageLayer)).r;\n"
        "    // TODO: it is /2 only for sub-sampled formats\n"
        "    vec2 CbCr = imageLoad(inputImageCbCr, ivec3(srcPos/2, pushConstants.srcImageLayer)).rg;\n"
        "\n"
        "    vec3 ycbcr = shiftCbCr(normalizeYCbCr(vec3(Y, CbCr)));\n"
        "    vec4 rgba = vec4(convertYCbCrToRgb(ycbcr),1.0);\n"
        "    // Store it back.\n"
        "    imageStore(outputImageRGB, ivec3(pos, pushConstants.dstImageLayer), rgba);\n"
        "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        std::cout << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

size_t VulkanFilterYuvCompute::InitYCBCRCOPY(std::string& computeShader)
{
    // The compute filter uses two or three input images as separate planes
    // Y (R) binding = 1
    // 2-planar: CbCr (RG) binding = 2
    // OR
    // 3-planar: Cb (R) binding = 2
    // 3-planar: Cr (R) binding = 3

    // The compute filter uses two or three output images as separate planes
    // Y (R) binding = 5
    // 2-planar: CbCr (RG) binding = 6
    // OR
    // 3-planar: Cb (R) binding = 6
    // 3-planar: Cr (R) binding = 7

    std::stringstream shaderStr;
    GenHeaderAndPushConst(shaderStr);
    // Input image
    shaderStr << " // The input image binding\n";
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   m_inputImageAspects,
                                   "inputImage",
                                   m_inputFormat,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   true  // imageArray
                                   );

    // Output image
    shaderStr << " // The output image binding\n";
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   m_outputImageAspects,
                                   "outputImage",
                                   m_outputFormat,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true   // imageArray
                                   );
    shaderStr << "\n\n";

    shaderStr <<
        "void main()\n"
        "{\n";
    GenHandleImagePosition(shaderStr);
    GenHandleSourcePositionWithReplicate(shaderStr, m_enableRowAndColumnReplication);
    shaderStr <<
        "    // Read Y value from source Y plane and write it to destination Y plane\n"
        "    float Y = imageLoad(inputImageY, ivec3(srcPos, pushConstants.srcImageLayer)).r;\n"
        "    imageStore(outputImageY, ivec3(pos, pushConstants.dstImageLayer), vec4(Y, 0, 0, 1));\n"
        "\n"
        "    // Do the same for the CbCr plane, but remember about the 4:2:0 subsampling\n"
        "    if (srcPos % 2 == ivec2(0, 0)) {\n"
        "        srcPos /= 2;\n"
        "        pos /= 2;\n"
        "        vec2 CbCr = imageLoad(inputImageCbCr, ivec3(srcPos, pushConstants.srcImageLayer)).rg;\n"
        "        imageStore(outputImageCbCr, ivec3(pos, pushConstants.dstImageLayer), vec4(CbCr, 0, 1));\n"
        "    }\n"
        "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        std::cout << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

size_t VulkanFilterYuvCompute::InitResize(std::string& computeShader)
{
    // The compute filter uses two input images as separate planes
    // Y (R) binding = 1
    // CbCr (RG) binding = 2
    // TODO: Add more YCbCr formats
    m_inputImageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

    // The compute filter uses two output images as separate planes
    // Y (R) binding = 5
    // CbCr (RG) binding = 6
    // TODO: Add more YCbCr formats
    m_outputImageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

    std::stringstream shaderStr;
    // Create compute pipeline
    shaderStr << "#version 450\n"
                        "layout(push_constant) uniform PushConstants {\n"
                        "    uint srcImageLayer;\n"
                        "    uint dstImageLayer;\n"
                        "    uint width;\n"
                        "    uint height;\n"
                        "    float scalingFactorX;\n"
                        "    float scalingFactorY;\n"
                        "} pushConstants;\n"
                        "\n"
                        "layout (local_size_x = 16, local_size_y = 16) in;\n"
                        " // TODO: use set and binding from the layout\n"
                        " // TODO: use r16 for 16-bit formats\n"
                        "layout (set = 0, binding = 1) uniform sampler2D texSampler;\n"
                        "layout (set = 0, binding = 2) uniform sampler2D texSamplerUV;\n"
                        " // TODO: use rgba16 for 16-bit formats\n"
                        "layout (set = 0, binding = 5, r8) uniform  writeonly image2DArray outImageY;\n"
                        " // TODO: use rg16 for 16-bit formats\n"
                        "layout (set = 0, binding = 6, rg8) uniform writeonly image2DArray outImageCbCr;\n"
                        "\n"
                        "\n";

    shaderStr <<
        "void main()\n"
        "{\n"
        "    ivec2 uvpos = ivec2(gl_GlobalInvocationID.xy);\n"
        "    ivec2 pos = uvpos *2; \n"
        "    vec2 scalingFactor = vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY);\n"
        "    vec2 sourcepos00 = vec2(vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY) * vec2(pos));\n"
        "    vec2 sourcepos10 = vec2(vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY) * vec2(pos.x+1, pos.y));\n"
        "    vec2 sourcepos01 = vec2(vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY) * vec2(pos.x, pos.y+1));\n"
        "    vec2 sourcepos11 = vec2(vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY) * vec2(pos.x+1, pos.y+1));\n"
        "\n"
        "    // Read Y value from source Y plane and write it to destination Y plane\n"
        "    float Y00 = texture(texSampler, sourcepos00).r;\n"
        "    float Y10 = texture(texSampler, sourcepos10).r;\n"
        "    float Y01 = texture(texSampler, sourcepos01).r;\n"
        "    float Y11 = texture(texSampler, sourcepos11).r;\n"
        "    imageStore(outImageY, ivec3(pos, pushConstants.dstImageLayer), vec4(Y00, 0, 0, 1));\n"
        "    imageStore(outImageY, ivec3(ivec2(pos.x+1, pos.y), pushConstants.dstImageLayer), vec4(Y10, 0, 0, 1));\n"
        "    imageStore(outImageY, ivec3(ivec2(pos.x, pos.y+1), pushConstants.dstImageLayer), vec4(Y01, 0, 0, 1));\n"
        "    imageStore(outImageY, ivec3(ivec2(pos.x+1, pos.y+1), pushConstants.dstImageLayer), vec4(Y11, 0, 0, 1));\n"
        "\n"
        "    // Do the same for the CbCr plane, but remember about the 4:2:0 subsampling\n"
        "    vec2 sourceposuv = ivec2(vec2(pushConstants.scalingFactorX, pushConstants.scalingFactorY) * vec2(uvpos));\n"
        "    vec2 CbCr = texture(texSamplerUV, sourceposuv).rg;\n"
        "    imageStore(outImageCbCr, ivec3(uvpos, pushConstants.dstImageLayer), vec4(CbCr, 0, 1));\n"
        "}\n";

    computeShader = shaderStr.str();
    std::cout << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

size_t VulkanFilterYuvCompute::InitYCBCRCLEAR(std::string& computeShader)
{
    // The compute filter uses NO input images
    m_inputImageAspects = VK_IMAGE_ASPECT_NONE;

    // The compute filter uses two or three output images as separate planes
    // Y (R) binding = 5
    // 2-planar: CbCr (RG) binding = 6
    // OR
    // 3-planar: Cb (R) binding = 6
    // 3-planar: Cr (R) binding = 7

    // Create compute pipeline
    std::stringstream shaderStr;
    GenHeaderAndPushConst(shaderStr);

    // Output image
    shaderStr << " // The output image binding\n";
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   m_outputImageAspects,
                                   "outputImage",
                                   m_outputFormat,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true   // imageArray
                                   );
    shaderStr << "\n\n";

    shaderStr <<
        "void main()\n"
        "{\n";
    GenHandleImagePosition(shaderStr);
    shaderStr <<
        "    imageStore(outputImageY, ivec3(pos, pushConstants.dstImageLayer), vec4(0.5, 0, 0, 1));\n"
        "\n"
        "    // Do the same for the CbCr plane, but remember about the 4:2:0 subsampling\n"
        "    if (pos % 2 == ivec2(0, 0)) {\n"
        "        pos /= 2;\n"
        "        imageStore(outputImageCbCr, ivec3(pos, pushConstants.dstImageLayer), vec4(0.5, 0.5, 0.0, 1.0));\n"
        "    }\n"
        "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        std::cout << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}
