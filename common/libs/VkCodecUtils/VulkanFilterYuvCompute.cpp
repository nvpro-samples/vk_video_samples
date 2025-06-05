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
                                        bool inputEnableMsbToLsbShift,
                                        bool outputEnableLsbToMsbShift,
                                        const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                                        const YcbcrPrimariesConstants* pYcbcrPrimariesConstants,
                                        const VkSamplerCreateInfo* pSamplerCreateInfo,
                                        VkSharedBaseObj<VulkanFilter>& vulkanFilter)
{

    VkSharedBaseObj<VulkanFilterYuvCompute> yCbCrVulkanFilter(new VulkanFilterYuvCompute(vkDevCtx,
                                                                                         queueFamilyIndex,
                                                                                         queueIndex,
                                                                                         flterType,
                                                                                         maxNumFrames,
                                                                                         inputFormat,
                                                                                         outputFormat,
                                                                                         inputEnableMsbToLsbShift,
                                                                                         outputEnableLsbToMsbShift,
                                                                                         pYcbcrPrimariesConstants));

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
    VkDescriptorType type = (ccSampler != VK_NULL_HANDLE) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkSampler* pImmutableSamplers = (ccSampler != VK_NULL_HANDLE) ? &ccSampler : nullptr;

    std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;

    // Input bindings (either images or buffers)
    if (m_inputIsBuffer) {
        // Binding 0: Input buffer (read-only) for single buffer case
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 1: Input buffer (read-only) Y plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 2: Input buffer (read-only) Cb or CbCr plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 3: Input buffer (read-only) Cr plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    } else {
        // Binding 0: Input image (read-only) RGBA or RGBA YCbCr sampler sampled
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 0, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, pImmutableSamplers});
        // Binding 1: Input image (read-only) Y plane of YCbCr Image
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 2: Input image (read-only) Cb or CbCr plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 3: Input image (read-only) Cr plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }

    // Output bindings (either images or buffers)
    if (m_outputIsBuffer) {
        // Binding 4: Output buffer (write) for single buffer case
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 5: Output buffer (write) Y plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 6: Output buffer (write) CbCr plane of 2-plane or Cb of 3-plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 7: Output buffer (write) Cr plane of 3-plane
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    } else {
        // Binding 4: Output image (write) RGBA or YCbCr single-plane image
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 5: Output image (write) Y plane of YCbCr Image
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 6: Output image (write) CbCr plane of 2-plane or Cb of 3-plane YCbCr Image
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        // Binding 7: Output image (write) Cr plane of 3-pane YCbCr Image
        setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }

    // Binding 8: uniform buffer for input parameters.
    setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});

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

// Generate a unified push constants declaration for shaders
/**
 * @brief Generates GLSL code for push constants declaration used in compute shaders
 *
 * This function creates a standard push constants block with fields for:
 * - Source and destination image layers
 * - Input and output dimensions
 * - Buffer offsets and pitches for Y, Cb, and Cr planes
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 */
static void GenPushConstantsDecl(std::stringstream& shaderStr) {
    shaderStr << "layout(push_constant) uniform PushConstants {\n"
              << "    uint srcLayer;        // src image layer to use\n"
              << "    uint dstLayer;        // dst image layer to use\n"
              << "    uint inputWidth;      // input image or buffer width\n"
              << "    uint inputHeight;     // input image or buffer height\n"
              << "    uint outputWidth;     // output image or buffer width\n"
              << "    uint outputHeight;    // output image or buffer height\n"
              << "    uint inYOffset;       // input  buffer Y plane offset\n"
              << "    uint inCbOffset;      // input  buffer Cb plane offset\n"
              << "    uint inCrOffset;      // input  buffer Cr plane offset\n"
              << "    uint inYPitch;        // input  buffer Y plane pitch\n"
              << "    uint inCbPitch;       // input  buffer Cb plane pitch\n"
              << "    uint inCrPitch;       // input  buffer Cr plane pitch\n"
              << "    uint outYOffset;      // output buffer Y plane offset\n"
              << "    uint outCbOffset;     // output buffer Cb plane offset\n"
              << "    uint outCrOffset;     // output buffer Cr plane offset\n"
              << "    uint outYPitch;       // output buffer Y plane pitch\n"
              << "    uint outCbPitch;      // output buffer Cb plane pitch\n"
              << "    uint outCrPitch;      // output buffer Cr plane pitch\n"
              << "} pushConstants;\n";
}

// Updated header function with unified push constants
/**
 * @brief Generates the shader header with version declaration and push constants
 *
 * Creates the beginning of a GLSL compute shader with:
 * - GLSL version declaration (#version 450)
 * - Push constants structure
 * - Local work group size (16x16)
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 */
static void GenHeaderAndPushConst(std::stringstream& shaderStr)
{
    shaderStr << "#version 450\n";
    GenPushConstantsDecl(shaderStr);
    shaderStr << "\n"
              << "layout (local_size_x = 16, local_size_y = 16) in;\n"
              << "\n";
}

/**
 * @brief Generates GLSL code for image binding layout declarations
 *
 * Creates the binding declaration for an image resource in the shader.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param imageName Base name for the image variable
 * @param imageSubName Suffix name for the image variable (e.g., "Y", "CbCr")
 * @param imageFormat Format string for the image (e.g., "rgba8")
 * @param isInput Whether this is an input (readonly) or output (writeonly) image
 * @param binding Binding point in the descriptor set
 * @param set Descriptor set number
 * @param imageArray Whether the image should be declared as image2DArray instead of image2D
 */
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

/**
 * @brief Generates GLSL code for handling global invocation position and bounds checking
 *
 * Creates code to:
 * - Get the current pixel position from gl_GlobalInvocationID
 * - Check if the position is within output image bounds
 * - Return early if out of bounds to prevent invalid memory access
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 */
static void GenHandleImagePosition(std::stringstream& shaderStr)
{
    shaderStr <<
    "    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n"
    "    // Check for out-of-bounds writes\n"
    "    if ((pos.x >= pushConstants.outputWidth) || (pos.y >= pushConstants.outputHeight)) {\n"
    "        return;\n"
    "    }\n"
    "\n";
}

/**
 * @brief Generates GLSL code for buffer binding layout declarations
 *
 * Creates the binding declaration for a buffer resource in the shader.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param bufferName Base name for the buffer variable
 * @param bufferSubName Suffix name for the buffer variable (e.g., "Y", "CbCr")
 * @param bufferDataType Data type of buffer elements (e.g., "uint8_t", "uint16_t")
 * @param bufferType Vulkan descriptor type (Storage buffer, uniform texel buffer, etc.)
 * @param isInput Whether this is an input (readonly) or output (writeonly) buffer
 * @param binding Binding point in the descriptor set
 * @param set Descriptor set number
 */
static void GenBufferIoBindingLayout(std::stringstream& shaderStr,
                                     const char *bufferName,
                                     const char *bufferSubName,
                                     const char *bufferDataType,
                                     VkDescriptorType bufferType,
                                     bool isInput,
                                     uint32_t binding,
                                     uint32_t set) {

    const char* readonlyModifier = isInput ? " readonly" : "";
    const char* writeonlyModifier = isInput ? "" : " writeonly";

    switch (bufferType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            shaderStr << "layout (set = " << set << ", binding = " << binding << ") uniform"
                      << " samplerBuffer "
                      << bufferName << bufferSubName
                      << ";\n";
            break;

        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            shaderStr << "layout (set = " << set << ", binding = " << binding << ") uniform"
                      << readonlyModifier << writeonlyModifier
                      << " imageBuffer "
                      << bufferName << bufferSubName
                      << ";\n";
            break;

        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            shaderStr << "layout (set = " << set << ", binding = " << binding << ") buffer"
                      << readonlyModifier << writeonlyModifier
                      << " " << bufferName << bufferSubName << "Buffer"
                      << " {\n"
                      << "    " << bufferDataType << "[] data;\n"
                      << "} " << bufferName << bufferSubName << ";\n";
            break;

        default:
            // Unsupported buffer type
            break;
    }
}

/**
 * @brief Generates GLSL code for determining if a position has chroma information
 *
 * Creates a condition that checks if the current pixel position contains
 * chroma information based on the subsampling ratios. For example, in 4:2:0
 * subsampling, only pixels at even x and y coordinates have chroma samples.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal subsampling ratio (1 for 4:4:4, 2 for 4:2:2/4:2:0)
 * @param chromaVertRatio Vertical subsampling ratio (1 for 4:4:4/4:2:2, 2 for 4:2:0)
 * @param useCondition Whether to output as a full if-condition (true) or just the condition expression (false)
 * @param pixelPosName Name of the pixel position variable in the shader (default: "srcPos")
 * @param setProcessChromaBool Name of the boolean variable to set (default: "processChromaBool")
 */
static void GenHandleChromaPosition(std::stringstream& shaderStr,
                                uint32_t chromaHorzRatio,
                                uint32_t chromaVertRatio,
                                bool useCondition = true,
                                const char* pixelPosName = "srcPos",
                                const char* setProcessChromaBool = "processChromaBool")
{
    // Skip this for 4:4:4 since all pixels have chroma
    if (chromaHorzRatio <= 1 && chromaVertRatio <= 1) {
        if (useCondition) {
            // For 4:4:4, no subsampling check needed - process all pixels
            shaderStr << "    bool " << setProcessChromaBool << " = true;\n";
        } else {
            shaderStr << "true";
        }
        return;
    }

    // Build condition for chroma sampling
    std::stringstream condition;
    if (chromaHorzRatio > 1)
        condition << "(" << pixelPosName << ".x % " << chromaHorzRatio << " == 0)";

    if (chromaHorzRatio > 1 && chromaVertRatio > 1)
        condition << " && ";

    if (chromaVertRatio > 1)
        condition << "(" << pixelPosName << ".y % " << chromaVertRatio << " == 0)";

    if (useCondition) {
        shaderStr << "    bool " << setProcessChromaBool << " = " << condition.str() << ";\n";
    } else {
        shaderStr << condition.str();
    }
}

/**
 * @brief Generates GLSL code for calculating subsampled chroma positions
 *
 * Creates code to compute the chroma position from a pixel position
 * based on the subsampling ratios. For example, in 4:2:0 subsampling,
 * the chroma position is calculated by dividing both x and y by 2.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal subsampling ratio (1 for 4:4:4, 2 for 4:2:2/4:2:0)
 * @param chromaVertRatio Vertical subsampling ratio (1 for 4:4:4/4:2:2, 2 for 4:2:0)
 * @param srcPosName Name of the source position variable (default: "srcPos")
 * @param dstPosName Name of the destination position variable (default: "chromaSrcPos")
 * @param indent Number of spaces to indent the output code (default: 8)
 * @param generateIfBlock Whether to generate an if-block or just assignment statements (default: false)
 */
static void GenCalculateChromaPosition(std::stringstream& shaderStr,
                                     uint32_t chromaHorzRatio,
                                     uint32_t chromaVertRatio,
                                     const char* srcPosName = "srcPos",
                                     const char* dstPosName = "chromaSrcPos",
                                     int indent = 8,
                                     bool generateIfBlock = false)
{
    std::string indentStr(indent, ' ');

    // For 4:4:4, no subsampling needed
    if (chromaHorzRatio <= 1 && chromaVertRatio <= 1) {
        shaderStr << indentStr << "// No subsampling for 4:4:4 format, use original position\n";
        if (generateIfBlock) {
            shaderStr << indentStr << "// " << dstPosName << " already equals " << srcPosName << "\n";
        } else {
            shaderStr << indentStr << dstPosName << " = " << srcPosName << ";\n";
        }
        return;
    }

    shaderStr << indentStr << "// Calculate subsampled positions based on format's subsampling\n";

    if (generateIfBlock) {
        // Generate an if-block for conditional calculation
        shaderStr << indentStr << dstPosName << " = " << srcPosName << ";\n";
        shaderStr << indentStr << "if (processChroma) {\n";

        if (chromaHorzRatio > 1) {
            shaderStr << indentStr << "    " << dstPosName << ".x = " << srcPosName << ".x / " << chromaHorzRatio << ";\n";
        }

        if (chromaVertRatio > 1) {
            shaderStr << indentStr << "    " << dstPosName << ".y = " << srcPosName << ".y / " << chromaVertRatio << ";\n";
        }

        shaderStr << indentStr << "}\n";
    } else {
        // Generate direct assignment statements
        shaderStr << indentStr << dstPosName << " = ivec2(";

        if (chromaHorzRatio > 1)
            shaderStr << srcPosName << ".x / " << chromaHorzRatio;
        else
            shaderStr << srcPosName << ".x";

        shaderStr << ", ";

        if (chromaVertRatio > 1)
            shaderStr << srcPosName << ".y / " << chromaVertRatio;
        else
            shaderStr << srcPosName << ".y";

        shaderStr << ");\n";
    }
}

/**
 * @brief Generates GLSL code for handling buffer position calculations with chroma subsampling
 *
 * Creates code to:
 * - Get the current pixel position from gl_GlobalInvocationID
 * - Check if the position is within output bounds
 * - Calculate appropriate buffer indices based on subsampling ratios
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal subsampling ratio (default: 2 for 4:2:0/4:2:2)
 * @param chromaVertRatio Vertical subsampling ratio (default: 2 for 4:2:0)
 */
static void GenHandleBufferPosition(std::stringstream& shaderStr, int chromaHorzRatio = 2, int chromaVertRatio = 2)
{
    shaderStr <<
    "    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n"
    "    // Check for out-of-bounds writes\n"
    "    if ((pos.x >= pushConstants.outputWidth) || (pos.y >= pushConstants.outputHeight)) {\n"
    "        return;\n"
    "    }\n"
    "    \n"
    "    // Calculate buffer indices based on position and strides\n"
    "    uint yIndex = pushConstants.inYOffset + pos.y * pushConstants.inYPitch + pos.x;\n"
    "    uint cbIndex = pushConstants.inCbOffset + (pos.y / " << chromaVertRatio << ") * pushConstants.inCbPitch + (pos.x / " << chromaHorzRatio << ");\n"
    "    uint crIndex = pushConstants.inCrOffset + (pos.y / " << chromaVertRatio << ") * pushConstants.inCrPitch + (pos.x / " << chromaHorzRatio << ");\n"
    "\n";
}

/**
 * @brief Generates GLSL code for handling source position with optional replication
 *
 * Creates code to calculate source position, with optional boundary handling
 * by replicating edge pixels when coordinates exceed input dimensions.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param enableReplicate Whether to enable edge replication (clamp to edge)
 */
static void GenHandleSourcePositionWithReplicate(std::stringstream& shaderStr, bool enableReplicate)
{
    if (enableReplicate) {
        shaderStr <<
        "    ivec2 srcPos = min(pos, ivec2(pushConstants.inputWidth, pushConstants.inputHeight));\n"
        "\n";
    } else {
        shaderStr <<
        "    ivec2 srcPos = pos;\n"
        "\n";
    }
}

/**
 * @brief Generates GLSL function for fetching Y samples from a buffer
 *
 * Creates a helper function that reads Y samples from a buffer and
 * normalizes values to 0.0-1.0 range, handling different bit depths.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isHighBitDepth Whether the Y data is high bit depth (>8 bits)
 * @param bitDepth The bit depth of Y samples (8, 10, 12, or 16)
 */
static void GenFetchYFromBufferFunc(std::stringstream& shaderStr,
                                    bool isHighBitDepth, uint32_t bitDepth)
{
    shaderStr << "// Function to fetch Y component from buffer\n"
              << "float fetchYFromBuffer(uint index) {\n";

    if (isHighBitDepth) {
        shaderStr << "    uint16_t rawValue = inputBufferY.data[index];\n"
                  << "    return extractHighBitDepth(rawValue);\n";
    } else {
        shaderStr << "    uint8_t byteValue = inputBufferY.data[index];\n"
                  << "    return float(byteValue) / 255.0;\n";
    }

    shaderStr << "}\n\n";
}

/**
 * @brief Generates GLSL functions for fetching Cb and Cr samples from buffers
 *
 * Creates helper functions to read Cb and Cr chroma samples from buffers and
 * normalize values to 0.0-1.0 range, handling different bit depths.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isHighBitDepth Whether the chroma data is high bit depth (>8 bits)
 * @param bitDepth The bit depth of chroma samples (8, 10, 12, or 16)
 */
static void GenFetchCbCrFromBufferFunc(std::stringstream& shaderStr,
                                       bool isHighBitDepth, uint32_t bitDepth) {
    // Cb fetch function
    shaderStr << "// Function to fetch Cb component from buffer\n"
              << "float fetchCbFromBuffer(uint index) {\n";

    if (isHighBitDepth) {
        shaderStr << "    uint16_t rawValue = inputBufferCb.data[index];\n"
                  << "    return extractHighBitDepth(rawValue);\n";
    } else {
        shaderStr << "    uint8_t byteValue = inputBufferCb.data[index];\n"
                  << "    return float(byteValue) / 255.0;\n";
    }

    shaderStr << "}\n\n";

    // Cr fetch function
    shaderStr << "// Function to fetch Cr component from buffer\n"
              << "float fetchCrFromBuffer(uint index) {\n";

    if (isHighBitDepth) {
        shaderStr << "    uint16_t rawValue = inputBufferCr.data[index];\n"
                  << "    return extractHighBitDepth(rawValue);\n";
    } else {
        shaderStr << "    uint8_t byteValue = inputBufferCr.data[index];\n"
                  << "    return float(byteValue) / 255.0;\n";
    }

    shaderStr << "}\n\n";
}

/**
 * @brief Generates GLSL function for extracting and normalizing high bit-depth values
 *
 * Creates a helper function to extract and normalize values from high bit-depth
 * formats (10, 12, or 16 bits), handling MSB or LSB aligned data.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isMSB Whether the high bits are MSB-aligned (true) or LSB-aligned (false)
 * @param bitDepth The bit depth of the samples (10, 12, or 16)
 */
static void GenExtractHighBitDepthFunc(std::stringstream& shaderStr,
                                       bool isMSB, uint32_t bitDepth)
{
    shaderStr << "// Helper function to extract and normalize high bit-depth values\n";

    if (isMSB) {
        // For MSB-aligned data
        shaderStr << "float extractHighBitDepth(uint value) {\n"
                  << "    // For MSB-aligned " << bitDepth << "-bit data, shift right to extract the bits\n"
                  << "    uint extractedValue = value >> (16u - " << bitDepth << "u);\n"
                  << "    // Normalize to 0.0-1.0 range\n"
                  << "    return float(extractedValue) / " << ((1 << bitDepth) - 1) << ".0;\n"
                  << "}\n\n";
    } else {
        // For LSB-aligned data
        shaderStr << "float extractHighBitDepth(uint value) {\n"
                  << "    // For LSB-aligned " << bitDepth << "-bit data, mask to extract the bits\n"
                  << "    uint extractedValue = value & " << ((1 << bitDepth) - 1) << "u;\n"
                  << "    // Normalize to 0.0-1.0 range\n"
                  << "    return float(extractedValue) / " << ((1 << bitDepth) - 1) << ".0;\n"
                  << "}\n\n";
    }
}

/**
 * @brief Generates GLSL code for applying MSB-to-LSB bit shifting for high bit-depth content
 *
 * Creates code to convert MSB-aligned high bit-depth content to normalized values:
 * - For images (floating point): Divide by the appropriate factor
 * - For buffers (integer): Perform right bit shift operations
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isInputBuffer Whether the input is a buffer (true) or image (false)
 * @param inputBitDepth The bit depth of the input data (8, 10, 12, or 16)
 * @param imageAspects Image aspect flags indicating which planes are being processed
 */
static void GenApplyMsbToLsbShift(std::stringstream& shaderStr,
                                 bool isInputBuffer,
                                 uint32_t inputBitDepth,
                                 VkImageAspectFlags imageAspects)
{
    // Only apply for high bit-depth formats (10/12-bit)
    if ((inputBitDepth != 10) && (inputBitDepth != 12)) {
        return;
    }

    // Calculate shift amount based on bit depth
    uint32_t shiftAmount = 16 - inputBitDepth;
    float shiftFactor = static_cast<float>(1 << shiftAmount);

    shaderStr << "\n    // MSB-to-LSB shift for high bit-depth "
              << (isInputBuffer ? "buffer" : "image") << " data\n";

    if (isInputBuffer) {
        // For buffers, we use actual bit shifting operations on integer values
        shaderStr << "    // For high bit-depth data in buffers, we need to shift right by "
                  << shiftAmount << " bits to convert from MSB-aligned to actual values\n"
                  << "    // This is a right shift operation for integer values\n";

        // Build a condition mask based on which components are being read
        std::string maskCondition = "";
        bool needsOr = false;

        if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
            maskCondition += "YCbCrRawOut.x > 0.0";
            needsOr = true;
        }

        if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
            if (needsOr) maskCondition += " || ";
            maskCondition += "YCbCrRawOut.y > 0.0";
            needsOr = true;
        }

        if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
            if (needsOr) maskCondition += " || ";
            maskCondition += "YCbCrRawOut.z > 0.0";
        }

        // Only apply shift if there are values to shift
        if (!maskCondition.empty()) {
            shaderStr << "    if (" << maskCondition << ") {\n"
                      << "        // Convert from uint values to normalized float (for buffer inputs)\n";

            if (inputBitDepth == 10) {
                shaderStr << "        // For 10-bit: Convert 10-bit values [0-1023] to normalized [0-1]\n"
                          << "        const float normFactor = 1.0 / 1023.0;\n";
            } else { // 12-bit
                shaderStr << "        // For 12-bit: Convert 12-bit values [0-4095] to normalized [0-1]\n"
                          << "        const float normFactor = 1.0 / 4095.0;\n";
            }

            // Apply right shift with bit mask to extract the actual bit values
            // For 10-bit: (value >> 6) & 0x3FF = value / 64 (rounded down)
            // For 12-bit: (value >> 4) & 0xFFF = value / 16 (rounded down)
            shaderStr << "        // Apply right shift to convert from MSB-aligned to actual bit values\n";

            // Apply component-specific shifting based on which aspects are being read
            if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
                shaderStr << "        YCbCrRawOut.x = floor(YCbCrRawOut.x / " << shiftFactor
                          << ".0) * normFactor;\n";
            }

            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                shaderStr << "        YCbCrRawOut.y = floor(YCbCrRawOut.y / " << shiftFactor
                          << ".0) * normFactor;\n";
            }

            if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                shaderStr << "        YCbCrRawOut.z = floor(YCbCrRawOut.z / " << shiftFactor
                          << ".0) * normFactor;\n";
            }

            shaderStr << "    }\n";
        }
    } else {
        // For images, we're already working with normalized values, so we divide by shiftFactor
        shaderStr << "    // For high bit-depth data in images that are MSB-aligned,\n"
                  << "    // we need to divide by " << shiftFactor << " to get the proper normalized values\n";

        // Build a shift mask based on which components are being read
        std::string shiftMask = "vec3(";
        shiftMask += (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) ? "1.0, " : "0.0, ";
        shiftMask += (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) ? "1.0, " : "0.0, ";
        shiftMask += (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) ? "1.0"   : "0.0";
        shiftMask += ")";

        // Calculate reciprocal of shift factor (for multiplication instead of division)
        float shiftFactorRecip = 1.0f / shiftFactor;

        // Only apply shift to the components that were actually read
        shaderStr << "    // Apply multiplication by reciprocal instead of division (more efficient)\n"
                  << "    const float shiftFactorRecip = " << std::fixed << std::setprecision(8) << shiftFactorRecip << "f;\n"
                  << "    YCbCrRawOut = YCbCrRawOut * shiftFactorRecip * " << shiftMask << " + \n"
                  << "                  YCbCrRawOut * (vec3(1.0) - " << shiftMask << ");\n";
    }
}

/**
 * @brief Generates GLSL function for reading YCbCr data from either buffer or image sources
 *
 * Creates a function that reads YCbCr data from the appropriate source (buffer or image)
 * based on the input format configuration. Handles different bit depths and plane layouts.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isInputBuffer Whether the input is a buffer (true) or image (false)
 * @param inputBitDepth The bit depth of the input data (8, 10, 12, or 16)
 * @param isInputTwoPlane Whether the input has two planes (e.g., NV12) or three planes
 */
static void GenReadYCbCrBuffer(std::stringstream& shaderStr,
                               bool isInputBuffer,
                               uint32_t inputBitDepth,
                               bool isInputTwoPlane,
                               bool enableMsbToLsbShift = false,
                               VkImageAspectFlags imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                                 VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                                 VK_IMAGE_ASPECT_PLANE_2_BIT,
                               const char* useProcessChromaBool = "processChroma")
{
    // Generate function to read from either buffer or image
    shaderStr <<
        "// Function to read YCbCr data from input source (buffer or image)\n"
        "vec3 readYCbCrFromSource(ivec2 pos, ivec2 chromaPos, uint srcLayer, bool processChroma) {\n"
        "    // Initialize to YCbCr black values (for limited range)\n";

    // Set appropriate black values based on bit depth
    if (inputBitDepth == 8) {
        shaderStr << "    vec3 YCbCrRawOut = vec3(16.0/255.0, 128.0/255.0, 128.0/255.0);\n\n";
    } else if (inputBitDepth == 10) {
        shaderStr << "    vec3 YCbCrRawOut = vec3(64.0/1023.0, 512.0/1023.0, 512.0/1023.0);\n\n";
    } else if (inputBitDepth == 12) {
        shaderStr << "    vec3 YCbCrRawOut = vec3(256.0/4095.0, 2048.0/4095.0, 2048.0/4095.0);\n\n";
    } else if (inputBitDepth == 16) {
        shaderStr << "    vec3 YCbCrRawOut = vec3(4096.0/65535.0, 32768.0/65535.0, 32768.0/65535.0);\n\n";
    } else {
        // Default fallback
        shaderStr << "    vec3 YCbCrRawOut = vec3(16.0/255.0, 128.0/255.0, 128.0/255.0);\n\n";
    }

    if (isInputBuffer) {
        // Reading from buffer
        shaderStr << "    // Reading from buffer source\n";

        // Read Y component if PLANE_0_BIT is set
        if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
            shaderStr <<
                "    // Calculate buffer index for Y plane\n"
                "    uint yIndex = pushConstants.inYOffset + pos.y * pushConstants.inYPitch + pos.x;\n"
                "    YCbCrRawOut.x = fetchYFromBuffer(yIndex);\n\n";
        }

        // Read Cb/Cr components based on plane format and aspect flags
        if ((imageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) {
            // Add conditional check for chroma processing
            shaderStr << "    // Process chroma data conditionally\n"
                      << "    if (processChroma) {\n";

            if (isInputTwoPlane) {
                // Two-plane input buffer format with interleaved CbCr
                shaderStr << "        // Read interleaved CbCr data from 2-plane input buffer\n"
                          << "        uint cbcrIndex = pushConstants.inCbOffset + chromaPos.y * pushConstants.inCbPitch + chromaPos.x * 2;\n";

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    shaderStr << "        YCbCrRawOut.y = fetchCbFromBuffer(cbcrIndex);\n"
                              << "        YCbCrRawOut.z = fetchCrFromBuffer(cbcrIndex + 1);\n";
                }
            } else {
                // Three-plane input buffer format with separate Cb and Cr planes
                shaderStr << "        // Read separate Cb and Cr from 3-plane input buffer\n";

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    shaderStr << "        uint cbIndex = pushConstants.inCbOffset + chromaPos.y * pushConstants.inCbPitch + chromaPos.x;\n"
                              << "        YCbCrRawOut.y = fetchCbFromBuffer(cbIndex);\n";
                }

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                    shaderStr << "        uint crIndex = pushConstants.inCrOffset + chromaPos.y * pushConstants.inCrPitch + chromaPos.x;\n"
                              << "        YCbCrRawOut.z = fetchCrFromBuffer(crIndex);\n";
                }
            }

            // Close the conditional block
            shaderStr << "    }\n";
        }
    } else {
        // Reading from image
        shaderStr << "    // Reading from image source\n";

        // Read Y component if PLANE_0_BIT is set
        if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
            shaderStr << "    // Read Y value from Y plane\n"
                      << "    YCbCrRawOut.x = imageLoad(inputImageY, ivec3(pos, srcLayer)).r;\n\n";
        }

        // Read Cb/Cr components based on plane format and aspect flags
        if ((imageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) {
            // Add conditional check for chroma processing
            shaderStr << "    // Process chroma data conditionally\n"
                      << "    if (processChroma) {\n";

            if (isInputTwoPlane) {
                // Two-plane input image format with interleaved CbCr
                shaderStr << "        // Read interleaved CbCr data from 2-plane input image\n";

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    // For two-plane formats (NV12, etc.), both Cb and Cr are in the second plane
                    shaderStr << "        YCbCrRawOut.yz = imageLoad(inputImageCbCr, ivec3(chromaPos, srcLayer)).rg;\n";
                }
            } else {
                // Three-plane input image format with separate Cb and Cr planes
                shaderStr << "        // Read separate Cb and Cr from 3-plane input image\n";

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    shaderStr << "        YCbCrRawOut.y = imageLoad(inputImageCb, ivec3(chromaPos, srcLayer)).r; // Cb\n";
                }

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                    shaderStr << "        YCbCrRawOut.z = imageLoad(inputImageCr, ivec3(chromaPos, srcLayer)).r; // Cr\n";
                }
            }

            // Close the conditional block
            shaderStr << "    }\n";
        }
    }

    // Apply MSB-to-LSB shift if enabled
    if (enableMsbToLsbShift) {
        GenApplyMsbToLsbShift(shaderStr, isInputBuffer, inputBitDepth, imageAspects);
    }

    // Return the raw YCbCr values
    shaderStr <<
        "\n    return YCbCrRawOut;\n"
        "}\n\n";
}

/**
 * @brief Generates GLSL function for applying LSB-to-MSB bit shifting for high bit-depth content
 *
 * Creates code to convert normalized values to MSB-aligned high bit-depth content by
 * applying the appropriate bit shift. This function only handles the shift calculation,
 * not the actual I/O operations.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isOutputBuffer Whether the output is a buffer (true) or image (false)
 * @param outputBitDepth The bit depth of the output data (8, 10, 12, or 16)
 */
static void GenApplyLsbToMsbShift(std::stringstream& shaderStr,
                                  bool isOutputBuffer,
                                  uint32_t outputBitDepth)
{
    // Only apply for high bit-depth formats (10/12-bit)
    if ((outputBitDepth != 10) && (outputBitDepth != 12)) {
        // For 8-bit or 16-bit, no shift is needed - just use the input values directly
        shaderStr << "    // No bit-depth shift needed for " << outputBitDepth << "-bit format\n\n";
        return;
    }

    // Calculate shift amount based on bit depth
    uint32_t shiftAmount = 16 - outputBitDepth;
    float shiftFactor = static_cast<float>(1 << shiftAmount);

    shaderStr << "    // Apply LSB-to-MSB shift for high bit-depth "
              << (isOutputBuffer ? "buffer" : "image") << " data\n";

    if (isOutputBuffer) {
        // For buffers, we'll return unshifted values because the packing functions
        // handle the bit shifting during the actual write operation
        shaderStr << "    // For buffer output, shift will be applied during packing\n\n";
    } else {
        // For images, we need to multiply by shift factor to align bits properly
        // Calculate multiplication factor
        shaderStr << "    // For image output with " << outputBitDepth << "-bit, multiply by " << shiftFactor
                  << " to shift into the MSB\n"
                  << "    const float shiftFactorMultiplier = " << shiftFactor << ";\n"
                  << "    YCbCrRawIn = YCbCrRawIn * shiftFactorMultiplier;\n\n";
    }
}

/**
 * @brief Generates GLSL function for writing YCbCr data to either buffer or image destinations
 *
 * Creates a function that writes YCbCr data to the appropriate destination (buffer or image)
 * based on the output format configuration. Handles different bit depths and plane layouts.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isOutputBuffer Whether the output is a buffer (true) or image (false)
 * @param outputBitDepth The bit depth of the output data (8, 10, 12, or 16)
 * @param isOutputTwoPlane Whether the output format has two planes (e.g., NV12) or three planes
 */
static void GenWriteYCbCrBuffer(std::stringstream& shaderStr,
                                bool isOutputBuffer,
                                uint32_t outputBitDepth,
                                bool isOutputTwoPlane,
                                bool enableLsbToMsbShift = false,
                                VkImageAspectFlags imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                                  VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                                  VK_IMAGE_ASPECT_PLANE_2_BIT,
                                const char* useProcessChromaBool = "processChroma")
{
    // Generate function to write to either buffer or image
    shaderStr <<
        "// Function to write YCbCr data to output destination (buffer or image)\n"
        "void writeYCbCrToDestination(vec3 YCbCrRawIn, ivec2 pos, ivec2 chromaPos, uint dstLayer, bool processChroma) {\n";

    // Apply LSB-to-MSB shift if enabled - just transforms the values, doesn't do I/O
    if (enableLsbToMsbShift) {
        GenApplyLsbToMsbShift(shaderStr, isOutputBuffer, outputBitDepth);
    }

    if (isOutputBuffer) {
        // Writing to buffer
        shaderStr <<
            "    // Writing to buffer destination\n";

        // Write Y component if PLANE_0_BIT is set
        if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
            shaderStr <<
                "    // Calculate buffer index for Y plane\n"
                "    uint outYIndex = pushConstants.outYOffset + pos.y * pushConstants.outYPitch + pos.x;\n\n";

            // Handle normal Y component based on bit depth
            if (outputBitDepth > 8) {
                // For high bit-depth formats
                switch (outputBitDepth) {
                    case 10:
                        shaderStr << "    outputBufferY.data[outYIndex] = pack10BitTo16Bit(YCbCrRawIn.x);\n\n";
                        break;
                    case 12:
                        shaderStr << "    outputBufferY.data[outYIndex] = pack12BitTo16Bit(YCbCrRawIn.x);\n\n";
                        break;
                    case 16:
                    default:
                        // For 16-bit, direct value
                        shaderStr << "    outputBufferY.data[outYIndex] = uint16_t(clamp(YCbCrRawIn.x, 0.0, 65535.0));\n\n";
                        break;
                }
            } else {
                // For 8-bit formats
                shaderStr << "    outputBufferY.data[outYIndex] = uint8_t(clamp(YCbCrRawIn.x, 0.0, 255.0));\n\n";
            }
        }

        // Write Cb/Cr components based on plane format and aspect flags
        if ((imageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) {
            shaderStr << "    // Process chroma data conditionally\n"
                      << "    if (processChroma) {\n";

            if (isOutputTwoPlane) {
                // Two-plane output buffer format with interleaved CbCr
                shaderStr << "        // Write interleaved CbCr to 2-plane output buffer\n"
                          << "        uint outCbCrIndex = pushConstants.outCbOffset + chromaPos.y * pushConstants.outCbPitch + chromaPos.x * 2;\n";

                // Normal CbCr processing
                if (outputBitDepth > 8) {
                    // For high bit-depth formats with interleaved data
                    switch (outputBitDepth) {
                        case 10:
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCbCr.data[outCbCrIndex] = pack10BitTo16Bit(YCbCrRawIn.y);\n"
                                          << "        outputBufferCbCr.data[outCbCrIndex + 1] = pack10BitTo16Bit(YCbCrRawIn.z);\n";
                            }
                            break;
                        case 12:
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCbCr.data[outCbCrIndex] = pack12BitTo16Bit(YCbCrRawIn.y);\n"
                                          << "        outputBufferCbCr.data[outCbCrIndex + 1] = pack12BitTo16Bit(YCbCrRawIn.z);\n";
                            }
                            break;
                        case 16:
                        default:
                            // For 16-bit, direct values
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCbCr.data[outCbCrIndex] = uint16_t(clamp(YCbCrRawIn.y, 0.0, 65535.0));\n"
                                          << "        outputBufferCbCr.data[outCbCrIndex + 1] = uint16_t(clamp(YCbCrRawIn.z, 0.0, 65535.0));\n";
                            }
                            break;
                    }
                } else {
                    // For 8-bit formats
                    if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                        shaderStr << "        outputBufferCbCr.data[outCbCrIndex] = uint8_t(clamp(YCbCrRawIn.y, 0.0, 255.0));\n"
                                  << "        outputBufferCbCr.data[outCbCrIndex + 1] = uint8_t(clamp(YCbCrRawIn.z, 0.0, 255.0));\n";
                    }
                }
            } else {
                // Three-plane output buffer format with separate Cb and Cr planes
                shaderStr << "        // Write separate Cb and Cr to 3-plane output buffer\n";

                // Calculate indices for separate planes
                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    shaderStr << "        uint outCbIndex = pushConstants.outCbOffset + chromaPos.y * pushConstants.outCbPitch + chromaPos.x;\n";
                }

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                    shaderStr << "        uint outCrIndex = pushConstants.outCrOffset + chromaPos.y * pushConstants.outCrPitch + chromaPos.x;\n";
                }

                if (outputBitDepth > 8) {
                    // For high bit-depth formats
                    switch (outputBitDepth) {
                        case 10:
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCb.data[outCbIndex] = pack10BitTo16Bit(YCbCrRawIn.y);\n";
                            }
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                                shaderStr << "        outputBufferCr.data[outCrIndex] = pack10BitTo16Bit(YCbCrRawIn.z);\n";
                            }
                            break;
                        case 12:
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCb.data[outCbIndex] = pack12BitTo16Bit(YCbCrRawIn.y);\n";
                            }
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                                shaderStr << "        outputBufferCr.data[outCrIndex] = pack12BitTo16Bit(YCbCrRawIn.z);\n";
                            }
                            break;
                        case 16:
                        default:
                            // For 16-bit, direct values
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                                shaderStr << "        outputBufferCb.data[outCbIndex] = uint16_t(clamp(YCbCrRawIn.y, 0.0, 65535.0));\n";
                            }
                            if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                                shaderStr << "        outputBufferCr.data[outCrIndex] = uint16_t(clamp(YCbCrRawIn.z, 0.0, 65535.0));\n";
                            }
                            break;
                    }
                } else {
                    // For 8-bit formats
                    if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                        shaderStr << "        outputBufferCb.data[outCbIndex] = uint8_t(clamp(YCbCrRawIn.y, 0.0, 255.0));\n";
                    }
                    if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                        shaderStr << "        outputBufferCr.data[outCrIndex] = uint8_t(clamp(YCbCrRawIn.z, 0.0, 255.0));\n";
                    }
                }
            }

            shaderStr << "    }\n"; // Close conditional chroma processing
        }
    } else {
        // Writing to image
        shaderStr << "    // Writing to image destination\n";

        // Write Y component if PLANE_0_BIT is set
        if (imageAspects & VK_IMAGE_ASPECT_PLANE_0_BIT) {
            shaderStr << "    // Write Y component to Y plane\n"
                      << "    imageStore(outputImageY, ivec3(pos, dstLayer), vec4(YCbCrRawIn.x, 0, 0, 1));\n\n";
        }

        // Write Cb/Cr components if their aspect flags are set
        if ((imageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) {
            // Add conditional check for chroma processing
            shaderStr << "    // Process chroma data conditionally\n"
                      << "    if (processChroma) {\n";

            if (isOutputTwoPlane) {
                // Two-plane output image format with interleaved CbCr
                if ((imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) != 0) {
                    // Both Cb and Cr are needed
                    shaderStr << "        // Write interleaved CbCr to 2-plane output image\n"
                              << "        imageStore(outputImageCbCr, ivec3(chromaPos, dstLayer), "
                              << "vec4(YCbCrRawIn.y, YCbCrRawIn.z, 0, 1));\n";
                }
            } else {
                // Three-plane output image format with separate Cb and Cr planes
                shaderStr << "        // Write separate Cb and Cr to 3-plane output image\n";

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
                    shaderStr << "        imageStore(outputImageCb, ivec3(chromaPos, dstLayer), vec4(YCbCrRawIn.y, 0, 0, 1));\n";
                }

                if (imageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
                    shaderStr << "        imageStore(outputImageCr, ivec3(chromaPos, dstLayer), vec4(YCbCrRawIn.z, 0, 0, 1));\n";
                }
            }

            // Close the conditional block
            shaderStr << "    }\n";
        }
    }

    // End the function
    shaderStr << "}\n\n";
}

uint32_t VulkanFilterYuvCompute::ShaderGenerateImagePlaneDescriptors(std::stringstream& shaderStr,
                                                                     VkImageAspectFlags& imageAspects,
                                                                     const char *imageName,
                                                                     VkFormat    imageFormat,
                                                                     bool isInput,
                                                                     uint32_t startBinding,
                                                                     uint32_t set,
                                                                     bool imageArray)
{
    shaderStr << " // The " << (isInput ? "input" : "output") << " image binding\n";
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

            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                         VK_IMAGE_ASPECT_PLANE_2_BIT;

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
                                startBinding++,
                                set,
                                imageArray);
    }

    return startBinding;
}

uint32_t VulkanFilterYuvCompute::ShaderGenerateBufferPlaneDescriptors(std::stringstream& shaderStr,
                                                                      VkImageAspectFlags& imageAspects,
                                                                      const char *bufferName,
                                                                      VkFormat    bufferFormat,
                                                                      bool isInput,
                                                                      uint32_t startBinding,
                                                                      uint32_t set,
                                                                      VkDescriptorType bufferType)
{
    // Buffer binding follows the same pattern as image binding:
    // offset 0: Single RGBA buffer with all data
    // offset 1: Y plane buffer
    // offset 2: 2-planar CbCr buffer or 3-planar Cb buffer
    // offset 3: 3-planar Cr buffer
    const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(bufferFormat);

    // Determine element size based on format
    const char* elementType = "uint8_t";  // Default to 8-bit

    shaderStr << " // The " << (isInput ? "input" : "output") << " buffer binding\n";
    // Check format for higher bit depths (16-bit formats)
    const VkFormatDesc* formatInfo = vkFormatLookUp(bufferFormat);
    if (formatInfo && formatInfo->name) {
        if (strstr(formatInfo->name, "16") != nullptr ||
            strstr(formatInfo->name, "R16") != nullptr ||
            strstr(formatInfo->name, "10") != nullptr ||
            strstr(formatInfo->name, "12") != nullptr) {
            elementType = "uint16_t";  // Use 16-bit for 10/12/16-bit formats
        }
    }

    if (inputMpInfo) {
        // For multi-planar formats, define separate buffers for each plane

        // Y plane buffer (plane 0)
        GenBufferIoBindingLayout(shaderStr, bufferName, "Y",
                                 elementType,
                                 bufferType,
                                 isInput,
                                 ++startBinding,
                                 set);

        if (inputMpInfo->planesLayout.numberOfExtraPlanes == 1) {
            // 2-plane format (NV12, NV21, etc.)
            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

            GenBufferIoBindingLayout(shaderStr, bufferName, "CbCr",
                                     elementType,
                                     bufferType,
                                     isInput,
                                     ++startBinding,
                                     set);

        } else if (inputMpInfo->planesLayout.numberOfExtraPlanes == 2) {
            // 3-plane format (YUV 4:2:0, 4:2:2, 4:4:4, etc.)
            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                         VK_IMAGE_ASPECT_PLANE_2_BIT;

            GenBufferIoBindingLayout(shaderStr, bufferName, "Cb",
                                     elementType,
                                     bufferType,
                                     isInput,
                                     ++startBinding,
                                     set);

            GenBufferIoBindingLayout(shaderStr, bufferName, "Cr",
                                     elementType,
                                     bufferType,
                                     isInput,
                                     ++startBinding,
                                     set);
        }
    } else {
        // For single-plane formats (like RGBA)
        imageAspects = VK_IMAGE_ASPECT_COLOR_BIT;

        GenBufferIoBindingLayout(shaderStr, bufferName, "RGB",
                                 elementType,
                                 bufferType,
                                 isInput,
                                 startBinding++,
                                 set);
    }

    return startBinding;
}


uint32_t VulkanFilterYuvCompute::ShaderGeneratePlaneDescriptors(std::stringstream& shaderStr,
                                                                bool isInput,
                                                                uint32_t startBinding,
                                                                uint32_t set,
                                                                bool imageArray,
                                                                VkDescriptorType bufferType)
{

    if ((isInput && m_inputIsBuffer) || (!isInput && m_outputIsBuffer)) {

        return ShaderGenerateBufferPlaneDescriptors(shaderStr,
                                             isInput ? m_inputImageAspects : m_outputImageAspects,
                                             isInput ? "inputBuffer" : "outputBuffer",
                                             isInput ? m_inputFormat : m_outputFormat,
                                             isInput, // isInput
                                             startBinding,    // startBinding
                                             set,             // set
                                             bufferType);
    } else {

        return ShaderGenerateImagePlaneDescriptors(shaderStr,
                                            isInput ? m_inputImageAspects : m_outputImageAspects,
                                            isInput ? "inputImage" : "outputImage",
                                            isInput ? m_inputFormat : m_outputFormat,
                                            isInput,       // isInput
                                            startBinding,  // startBinding
                                            set,           // set
                                            imageArray  // imageArray
                                            );
    }
}

/**
 * @brief Generates GLSL functions for YCbCr normalization with different bit depths
 *
 * Creates helper functions to normalize YCbCr values, handling different bit depths,
 * and applying proper range adjustments (limited/full range).
 *
 * Process steps:
 * 1. Calculate normalization parameters based on bit depth and range
 * 2. Generate Y normalization function (scaling + offset)
 * 3. Generate CbCr shifting functions (centering around zero)
 * 4. Generate CbCr normalization functions (scaling + offset)
 * 5. Generate bit-depth specific helpers for 10/12-bit formats
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param bitDepth The bit depth of the YCbCr data (8, 10, 12, or 16)
 * @param isLimitedRange Whether values are limited range (true) or full range (false)
 * @param hasChroma Whether to include chroma normalization functions
 */
static void GenYCbCrNormalizationFuncs(std::stringstream& shaderStr,
                                       uint32_t bitDepth = 8,
                                       bool isLimitedRange = true,
                                       bool hasChroma = true)
{
    // STEP 1: Calculate normalization parameters based on bit depth and range
    // ===========================================================================

    // Use double precision for calculations to maintain precision
    double maxValue = (1ULL << bitDepth) - 1.0;  // Max value for the given bit depth

    // Limited range values for different bit depths
    double yBlack, yWhite, cZero, cScale;

    if (isLimitedRange) {
        // Step 1.1: Calculate limited range (aka TV/Video range) values
        // Use standard-compliant values for different bit depths
        switch (bitDepth) {
            case 10:
                // 10-bit limited range: Y[64,940], C[64,960]
                yBlack = 64.0;
                yWhite = 940.0;
                cZero = 64.0;
                cScale = 896.0;  // 960 - 64
                break;
            case 12:
                // 12-bit limited range: Y[256,3760], C[256,3840]
                yBlack = 256.0;
                yWhite = 3760.0;
                cZero = 256.0;
                cScale = 3584.0;  // 3840 - 256
                break;
            case 16:
                // 16-bit limited range: scale 8-bit values by 2^8
                yBlack = 16.0 * 256.0;
                yWhite = 235.0 * 256.0;
                cZero = 16.0 * 256.0;
                cScale = 224.0 * 256.0;
                break;
            case 8:
            default:
                // 8-bit limited range: Y[16,235], C[16,240]
                yBlack = 16.0;
                yWhite = 235.0;
                cZero = 16.0;
                cScale = 224.0;
                break;
        }
    } else {
        // Step 1.2: Calculate full range values (same for all bit depths, just scaled)
        yBlack = 0.0;
        yWhite = maxValue;
        cZero = 0.0;
        cScale = maxValue;
    }

    // Step 1.3: Calculate normalization factors with double precision
    double yRange = yWhite - yBlack;
    double yFactor = 1.0 / yRange;
    double yOffset = -yBlack * yFactor;
    double cFactor = 1.0 / cScale;

    // Format values with high precision for GLSL
    std::stringstream ss;
    ss.precision(16); // Use high precision for constants

    // STEP 2: Generate Y normalization function
    // ===========================================================================
    shaderStr << "\n"
              << "// Specify high precision for all floating point calculations\n"
              << "precision highp float;\n"
              << "precision highp int;\n"
              << "\n"
              << "// STEP 1: Normalize Y component for " << bitDepth << "-bit "
              << (isLimitedRange ? "limited range" : "full range") << " content\n"
              << "highp float normalizeY(highp float Y) {\n";

    if (isLimitedRange) {
        // Step 2.1: Limited range needs black level adjustment and scaling
        // Format with high precision
        ss.str("");
        ss << std::fixed << yFactor;
        std::string yFactorStr = ss.str();

        ss.str("");
        ss << std::fixed << yOffset;
        std::string yOffsetStr = ss.str();

        shaderStr << "    // Step 1.1: Map from [" << yBlack << ", " << yWhite << "] to [0.0, 1.0]\n"
                  << "    // Formula: normalizedY = (Y - yBlack) / yRange = Y * yFactor + yOffset\n"
                  << "    return Y * " << yFactorStr << " + " << yOffsetStr << ";\n";
    } else {
        // Step 2.2: Full range just needs scaling
        shaderStr << "    // Step 1.1: Map from [0, " << maxValue << "] to [0.0, 1.0]\n"
                  << "    // Formula: normalizedY = Y / maxValue\n"
                  << "    return Y / " << maxValue << ";\n";
    }
    shaderStr << "}\n\n";

    if (hasChroma) {
        // STEP 3: Generate CbCr shifting functions
        // ===========================================================================

        // Step 3.1: Generate CbCr shifting function for vec2 (common for 2-plane formats)
        shaderStr << "// STEP 2: Shift CbCr components from centered range to [-0.5, 0.5] range\n"
                  << "highp vec2 shiftCbCr(highp vec2 CbCr) {\n"
                  << "    // Step 2.1: Shift from [0.0, 1.0] to [-0.5, 0.5]\n"
                  << "    return CbCr - 0.5;\n"
                  << "}\n\n";

        // Step 3.2: Generate CbCr shifting function for vec3 (for full YCbCr triplet)
        shaderStr << "// Step 2 (alternative): Shift YCbCr components, leaving Y alone but centering CbCr\n"
                  << "highp vec3 shiftCbCr(highp vec3 ycbcr) {\n"
                  << "    // Step 2.1: Shift only Cb and Cr from [0.0, 1.0] to [-0.5, 0.5]\n"
                  << "    const highp vec3 shift = vec3(0.0, -0.5, -0.5);\n"
                  << "    return ycbcr + shift;\n"
                  << "}\n\n";

        // STEP 4: Generate CbCr normalization function
        // ===========================================================================
        shaderStr << "// STEP 3: Normalize CbCr components for " << bitDepth << "-bit "
                  << (isLimitedRange ? "limited range" : "full range") << " content\n"
                  << "highp vec2 normalizeCbCr(highp vec2 CbCr) {\n";

        if (isLimitedRange) {
            // Step 4.1: Limited range needs zero level adjustment and scaling
            // Format with high precision
            ss.str("");
            ss << std::fixed << cZero;
            std::string cZeroStr = ss.str();

            ss.str("");
            ss << std::fixed << cFactor;
            std::string cFactorStr = ss.str();

            shaderStr << "    // Step 3.1: Map from [" << cZero << ", " << (cZero + cScale) << "] to [0.0, 1.0]\n"
                      << "    // Formula: normalizedCbCr = (CbCr - cZero) / cScale\n"
                      << "    return (CbCr - " << cZeroStr << ") * " << cFactorStr << ";\n";
        } else {
            // Step 4.2: Full range just needs scaling
            shaderStr << "    // Step 3.1: Map from [0, " << maxValue << "] to [0.0, 1.0]\n"
                      << "    // Formula: normalizedCbCr = CbCr / maxValue\n"
                      << "    return CbCr / " << maxValue << ";\n";
        }
        shaderStr << "}\n\n";
    }

    // STEP 5: Generate bit-depth specific helper functions for 10/12-bit formats
    // ===========================================================================
    if (bitDepth == 10) {
        shaderStr << "// STEP 4: Special 10-bit format handling functions\n"
                  << "// 10-bit packing formats often store values in uint16 or uint32 with specific bit layouts\n"
                  << "\n"
                  << "// Extract 10-bit value from 16-bit storage (common for P010, P210, etc.)\n"
                  << "highp float extract10BitFrom16Bit(highp uint value) {\n"
                  << "    // Most 10-bit formats store the value in the most significant 10 bits\n"
                  << "    highp uint raw10bit = value >> 6; // Shift right to remove 6 padding bits\n"
                  << "    return float(raw10bit);\n"
                  << "}\n\n"

                  << "// Extract 10-bit value from 16-bit storage as normalized float\n"
                  << "highp float extract10BitNormalized(highp uint value) {\n"
                  << "    highp uint raw10bit = value >> 6; // Shift right to remove 6 padding bits\n"
                  << "    return float(raw10bit) / 1023.0; // Normalize to [0,1]\n"
                  << "}\n\n"

                  << "// Normalize packed 10-bit YUV directly\n"
                  << "highp vec3 normalize10BitYUV(highp uvec3 packedYuv) {\n"
                  << "    // Extract 10-bit components\n"
                  << "    highp float y = extract10BitFrom16Bit(packedYuv.x);\n"
                  << "    highp float cb = extract10BitFrom16Bit(packedYuv.y);\n"
                  << "    highp float cr = extract10BitFrom16Bit(packedYuv.z);\n"
                  << "    // Normalize components\n"
                  << "    y = normalizeY(y);\n"
                  << "    highp vec2 cbcr = normalizeCbCr(vec2(cb, cr));\n"
                  << "    return vec3(y, cbcr);\n"
                  << "}\n\n";
    } else if (bitDepth == 12) {
        shaderStr << "// STEP 4: Special 12-bit format handling functions\n"
                  << "// 12-bit packing formats often store values in uint16 or uint32 with specific bit layouts\n"
                  << "\n"
                  << "// Extract 12-bit value from 16-bit storage (common for P012, P212, etc.)\n"
                  << "highp float extract12BitFrom16Bit(highp uint value) {\n"
                  << "    // Most 12-bit formats store the value in the most significant 12 bits\n"
                  << "    highp uint raw12bit = value >> 4; // Shift right to remove 4 padding bits\n"
                  << "    return float(raw12bit);\n"
                  << "}\n\n"

                  << "// Extract 12-bit value from 16-bit storage as normalized float\n"
                  << "highp float extract12BitNormalized(highp uint value) {\n"
                  << "    highp uint raw12bit = value >> 4; // Shift right to remove 4 padding bits\n"
                  << "    return float(raw12bit) / 4095.0; // Normalize to [0,1]\n"
                  << "}\n\n"

                  << "// Normalize packed 12-bit YUV directly\n"
                  << "highp vec3 normalize12BitYUV(highp uvec3 packedYuv) {\n"
                  << "    // Extract 12-bit components\n"
                  << "    highp float y = extract12BitFrom16Bit(packedYuv.x);\n"
                  << "    highp float cb = extract12BitFrom16Bit(packedYuv.y);\n"
                  << "    highp float cr = extract12BitFrom16Bit(packedYuv.z);\n"
                  << "    // Normalize components\n"
                  << "    y = normalizeY(y);\n"
                  << "    highp vec2 cbcr = normalizeCbCr(vec2(cb, cr));\n"
                  << "    return vec3(y, cbcr);\n"
                  << "}\n\n";
    }
}

/**
 * @brief Generates GLSL functions for YCbCr denormalization with different bit depths
 *
 * Creates helper functions to denormalize YCbCr values from normalized [0-1] for Y and
 * [-0.5,0.5] for CbCr back to the appropriate bit depth and range (limited or full).
 * This is the inverse operation of GenYCbCrNormalizationFuncs.
 *
 * Process steps:
 * 1. Calculate denormalization parameters based on bit depth and range
 * 2. Generate Y denormalization function (inverse scaling + offset)
 * 3. Generate CbCr unshifting functions (recentering to [0,1])
 * 4. Generate CbCr denormalization functions (inverse scaling + offset)
 * 5. Generate combined convenience functions
 * 6. Generate bit-depth specific packing helpers for 10/12-bit formats
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param bitDepth The target bit depth for the YCbCr data (8, 10, 12, or 16)
 * @param isLimitedRange Whether target values are limited range (true) or full range (false)
 * @param hasChroma Whether to include chroma denormalization functions
 */
static void GenYCbCrDeNormalizationFuncs(std::stringstream& shaderStr,
                                         uint32_t bitDepth = 8,
                                         bool isLimitedRange = true,
                                         bool hasChroma = true)
{
    // STEP 1: Calculate denormalization parameters based on bit depth and range
    // ===========================================================================

    // Use double precision for calculations to maintain precision
    double maxValue = (1ULL << bitDepth) - 1.0;  // Max value for the given bit depth

    // Limited range values for different bit depths
    double yBlack, yWhite, cZero, cScale;

    if (isLimitedRange) {
        // Step 1.1: Calculate limited range (aka TV/Video range) values
        // Use standard-compliant values for different bit depths
        switch (bitDepth) {
            case 10:
                // 10-bit limited range: Y[64,940], C[64,960]
                yBlack = 64.0;
                yWhite = 940.0;
                cZero = 64.0;
                cScale = 896.0;  // 960 - 64
                break;
            case 12:
                // 12-bit limited range: Y[256,3760], C[256,3840]
                yBlack = 256.0;
                yWhite = 3760.0;
                cZero = 256.0;
                cScale = 3584.0;  // 3840 - 256
                break;
            case 16:
                // 16-bit limited range: scale 8-bit values by 2^8
                yBlack = 16.0 * 256.0;
                yWhite = 235.0 * 256.0;
                cZero = 16.0 * 256.0;
                cScale = 224.0 * 256.0;
                break;
            case 8:
            default:
                // 8-bit limited range: Y[16,235], C[16,240]
                yBlack = 16.0;
                yWhite = 235.0;
                cZero = 16.0;
                cScale = 224.0;
                break;
        }
    } else {
        // Step 1.2: Calculate full range values (same for all bit depths, just scaled)
        yBlack = 0.0;
        yWhite = maxValue;
        cZero = 0.0;
        cScale = maxValue;
    }

    // Step 1.3: Calculate denormalization factors (inverse of normalization)
    double yRange = yWhite - yBlack;

    // Format values with high precision for GLSL
    std::stringstream ss;
    ss.precision(16); // Use high precision for constants

    // STEP 2: Generate Y denormalization function
    // ===========================================================================
    shaderStr << "\n"
              << "// Specify high precision for all floating point calculations\n"
              << "precision highp float;\n"
              << "precision highp int;\n"
              << "\n"
              << "// STEP 1: Denormalize Y component from [0.0, 1.0] back to " << bitDepth << "-bit "
              << (isLimitedRange ? "limited range" : "full range") << " content\n"
              << "highp float denormalizeY(highp float normalizedY) {\n";

    if (isLimitedRange) {
        // Step 2.1: Limited range needs scaling and black level adjustment
        // Format with high precision
        ss.str("");
        ss << std::fixed << yRange;
        std::string yRangeStr = ss.str();

        ss.str("");
        ss << std::fixed << yBlack;
        std::string yBlackStr = ss.str();

        shaderStr << "    // Step 1.1: Map from [0.0, 1.0] back to [" << yBlack << ", " << yWhite << "]\n"
                  << "    // Formula: Y = normalizedY * yRange + yBlack\n"
                  << "    return normalizedY * " << yRangeStr << " + " << yBlackStr << ";\n";
    } else {
        // Step 2.2: Full range just needs scaling
        shaderStr << "    // Step 1.1: Map from [0.0, 1.0] back to [0, " << maxValue << "]\n"
                  << "    // Formula: Y = normalizedY * maxValue\n"
                  << "    return normalizedY * " << maxValue << ";\n";
    }
    shaderStr << "}\n\n";

    if (hasChroma) {
        // STEP 3: Generate CbCr unshifting function
        // ===========================================================================
        shaderStr << "// STEP 2: Unshift CbCr components from [-0.5, 0.5] range back to centered range [0.0, 1.0]\n"
                  << "highp vec2 unshiftCbCr(highp vec2 shiftedCbCr) {\n"
                  << "    // Step 2.1: Shift from [-0.5, 0.5] back to [0.0, 1.0]\n"
                  << "    return shiftedCbCr + 0.5;\n"
                  << "}\n\n";

        // STEP 4: Generate CbCr denormalization function
        // ===========================================================================
        shaderStr << "// STEP 3: Denormalize CbCr components from [0.0, 1.0] back to " << bitDepth << "-bit "
                  << (isLimitedRange ? "limited range" : "full range") << " content\n"
                  << "highp vec2 denormalizeCbCr(highp vec2 normalizedCbCr) {\n";

        if (isLimitedRange) {
            // Step 4.1: Limited range needs scaling and zero level adjustment
            // Format with high precision
            ss.str("");
            ss << std::fixed << cScale;
            std::string cScaleStr = ss.str();

            ss.str("");
            ss << std::fixed << cZero;
            std::string cZeroStr = ss.str();

            shaderStr << "    // Step 3.1: Map from [0.0, 1.0] back to [" << cZero << ", " << (cZero + cScale) << "]\n"
                      << "    // Formula: CbCr = normalizedCbCr * cScale + cZero\n"
                      << "    return normalizedCbCr * " << cScaleStr << " + " << cZeroStr << ";\n";
        } else {
            // Step 4.2: Full range just needs scaling
            shaderStr << "    // Step 3.1: Map from [0.0, 1.0] back to [0, " << maxValue << "]\n"
                      << "    // Formula: CbCr = normalizedCbCr * maxValue\n"
                      << "    return normalizedCbCr * " << maxValue << ";\n";
        }
        shaderStr << "}\n\n";

        // STEP 5: Generate combined convenience functions
        // ===========================================================================

        // Step 5.1: Combined unshift and denormalize
        shaderStr << "// STEP 4: Combined function: unshift and denormalize CbCr in one step\n"
                  << "highp vec2 unshiftAndDenormalizeCbCr(highp vec2 shiftedCbCr) {\n"
                  << "    // Step 4.1: First unshift from [-0.5, 0.5] to [0.0, 1.0], then denormalize\n"
                  << "    return denormalizeCbCr(unshiftCbCr(shiftedCbCr));\n"
                  << "}\n\n";

        // Step 5.2: Full YCbCr denormalization
        shaderStr << "// STEP 5: Combined function to denormalize full YCbCr triplet\n"
                  << "highp vec3 denormalizeYCbCr(highp vec3 normalizedYCbCr) {\n"
                  << "    // Step 5.1: Denormalize Y component\n"
                  << "    highp float y = denormalizeY(normalizedYCbCr.x);\n"
                  << "    // Step 5.2: Unshift and denormalize Cb and Cr components\n"
                  << "    highp vec2 cbcr = denormalizeCbCr(vec2(normalizedYCbCr.y + 0.5, normalizedYCbCr.z + 0.5));\n"
                  << "    // Step 5.3: Combine the components into a single vector\n"
                  << "    return vec3(y, cbcr);\n"
                  << "}\n\n";
    }

    // STEP 6: Generate bit-depth specific packing helpers for 10/12-bit formats
    // ===========================================================================
    if (bitDepth == 10) {
        shaderStr << "// STEP 6: Special 10-bit format packing functions\n"
                  << "// Pack 10-bit values into 16-bit storage (common for P010, P210, etc.)\n"
                  << "\n"
                  << "// Pack 10-bit value into 16-bit storage (MSB aligned with padding)\n"
                  << "highp uint pack10BitTo16Bit(highp float value) {\n"
                  << "    // Clamp the input value to the valid range for 10-bit\n"
                  << "    highp uint raw10bit = uint(clamp(value, 0.0, 1023.0));\n"
                  << "    // Shift left by 6 bits to store in MSB format (standard for P010, etc.)\n"
                  << "    return raw10bit << 6;\n"
                  << "}\n\n"

                  << "// Pack normalized [0,1] value into 10-bit MSB aligned format\n"
                  << "highp uint packNormalizedTo10Bit(highp float normalizedValue) {\n"
                  << "    // Scale to 10-bit range and pack\n"
                  << "    highp uint raw10bit = uint(clamp(normalizedValue * 1023.0, 0.0, 1023.0));\n"
                  << "    return raw10bit << 6;\n"
                  << "}\n\n"

                  << "// Pack denormalized YUV to 10-bit values\n"
                  << "highp uvec3 packYUVTo10Bit(highp vec3 yuv) {\n"
                  << "    // Denormalize components first\n"
                  << "    highp vec3 denormYuv = denormalizeYCbCr(yuv);\n"
                  << "    // Pack each component into 16-bit storage (MSB aligned)\n"
                  << "    return uvec3(\n"
                  << "        pack10BitTo16Bit(denormYuv.x),  // Y\n"
                  << "        pack10BitTo16Bit(denormYuv.y),  // Cb\n"
                  << "        pack10BitTo16Bit(denormYuv.z)   // Cr\n"
                  << "    );\n"
                  << "}\n\n";
    } else if (bitDepth == 12) {
        shaderStr << "// STEP 6: Special 12-bit format packing functions\n"
                  << "// Pack 12-bit values into 16-bit storage (common for P012, P212, etc.)\n"
                  << "\n"
                  << "// Pack 12-bit value into 16-bit storage (MSB aligned with padding)\n"
                  << "highp uint pack12BitTo16Bit(highp float value) {\n"
                  << "    // Clamp the input value to the valid range for 12-bit\n"
                  << "    highp uint raw12bit = uint(clamp(value, 0.0, 4095.0));\n"
                  << "    // Shift left by 4 bits to store in MSB format (standard for P012, etc.)\n"
                  << "    return raw12bit << 4;\n"
                  << "}\n\n"

                  << "// Pack normalized [0,1] value into 12-bit MSB aligned format\n"
                  << "highp uint packNormalizedTo12Bit(highp float normalizedValue) {\n"
                  << "    // Scale to 12-bit range and pack\n"
                  << "    highp uint raw12bit = uint(clamp(normalizedValue * 4095.0, 0.0, 4095.0));\n"
                  << "    return raw12bit << 4;\n"
                  << "}\n\n"

                  << "// Pack denormalized YUV to 12-bit values\n"
                  << "highp uvec3 packYUVTo12Bit(highp vec3 yuv) {\n"
                  << "    // Denormalize components first\n"
                  << "    highp vec3 denormYuv = denormalizeYCbCr(yuv);\n"
                  << "    // Pack each component into 16-bit storage (MSB aligned)\n"
                  << "    return uvec3(\n"
                  << "        pack12BitTo16Bit(denormYuv.x),  // Y\n"
                  << "        pack12BitTo16Bit(denormYuv.y),  // Cb\n"
                  << "        pack12BitTo16Bit(denormYuv.z)   // Cr\n"
                  << "    );\n"
                  << "}\n\n";
    }
}

/**
 * @brief Generates GLSL function for YCbCr format conversion with normalization and denormalization
 *
 * Creates a helper function for converting between different YCbCr formats
 * that normalizes input values, then denormalizes to the target format.
 * This handles both bit-depth and range conversions.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param inputBitDepth The bit depth of input YCbCr data (8, 10, 12, or 16 bits)
 * @param outputBitDepth The bit depth of output YCbCr data (8, 10, 12, or 16 bits)
 * @param isInputLimitedRange Whether the input uses limited range (true) or full range (false)
 * @param isOutputLimitedRange Whether the output uses limited range (true) or full range (false)
 */
static void GenConvertYCbCrFormat(std::stringstream& shaderStr,
                                  uint32_t inputBitDepth = 8,
                                  uint32_t outputBitDepth = 8,
                                  bool isInputLimitedRange = true,
                                  bool isOutputLimitedRange = true)
{
    shaderStr <<
        "// Function to handle YCbCr format conversion with proper normalization\n"
        "vec3 convertYCbCrFormat(vec3 YCbCrRawIn) {\n"
        "    // Step 1: Normalize input YCbCr values to [0-1] range\n"
        "    float normalizedY = normalizeY(YCbCrRawIn.x);\n"
        "    vec2 normalizedCbCr = normalizeCbCr(vec2(YCbCrRawIn.y, YCbCrRawIn.z));\n\n"
        "    // Step 2: Denormalize to output bit depth and range\n"
        "    float y = denormalizeY(normalizedY);\n"
        "    vec2 cbcr = denormalizeCbCr(normalizedCbCr);\n\n"
        "    // Return the converted values\n"
        "    return vec3(y, cbcr.x, cbcr.y);\n"
        "}\n\n";
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

    // 1. Generate header and push constants
    GenHeaderAndPushConst(shaderStr);

    // 2. Generate IO bindings
    // Input image
    shaderStr << " // The input YCbCr input binding\n";
    // Input Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   true,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    shaderStr << " // The output RGBA image binding\n";
    // Output Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true,  // imageArray
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Get format information to determine bit depth
    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo =
        m_samplerYcbcrConversion.GetSamplerYcbcrConversionCreateInfo();
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(samplerYcbcrConversionCreateInfo.format);

    // Determine bit depth from the format
    uint32_t bitDepth = mpInfo ? GetBitsPerChannel(mpInfo->planesLayout) : 8;

    // Determine if we're using limited or full range
    bool isLimitedRange = (samplerYcbcrConversionCreateInfo.ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);

    // 3. Generate helper functions for YCbCr normalization with proper bit depth handling
    GenYCbCrNormalizationFuncs(shaderStr, bitDepth, isLimitedRange, true);

    // 4. Generate YCbCr to RGB conversion function
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

    // 5. Generate color range normalization function
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

    // 6. Generate function to fetch YCbCr components from images
    shaderStr <<
        "vec3 fetchYCbCrFromImage(ivec3 pos) {\n"
        "    // Fetch from the texture.\n"
        "    float Y = imageLoad(inputImageY, pos).r;\n"
        "    // For subsampled formats, divide by 2\n"
        "    vec2 CbCr = imageLoad(inputImageCbCr, ivec3(pos.xy/2, pos.z)).rg;\n"
        "    return vec3(Y, CbCr);\n"
        "}\n"
        "\n";

    // 7. Generate function to write RGBA to output image
    shaderStr <<
        "void writeRgbaToImage(vec4 rgba, ivec3 pos) {\n"
        "    imageStore(outputImageRGB, pos, rgba);\n"
        "}\n"
        "\n";

    // 8. Main function
    shaderStr <<
        "void main()\n"
        "{\n";

    // 9. Handle position calculation
    GenHandleImagePosition(shaderStr);

    // 10. Calculate source position with replication if enabled
    GenHandleSourcePositionWithReplicate(shaderStr, m_enableRowAndColumnReplication);

    // 11. YCbCr to RGB conversion
    shaderStr <<
        "    // Calculate position with layer\n"
        "    ivec3 srcPos3D = ivec3(srcPos, pushConstants.srcLayer);\n"
        "    ivec3 dstPos3D = ivec3(pos, pushConstants.dstLayer);\n"
        "\n"
        "    // Fetch YCbCr components\n"
        "    vec3 ycbcr = fetchYCbCrFromImage(srcPos3D);\n"
        "\n"
        "    // Process: normalize, shift, and convert to RGB\n"
        "    ycbcr = shiftCbCr(normalizeYCbCr(ycbcr));\n"
        "    vec3 rgb = convertYCbCrToRgb(ycbcr);\n"
        "\n"
        "    // Write final RGBA result\n"
        "    vec4 rgba = vec4(rgb, 1.0);\n"
        "    writeRgbaToImage(rgba, dstPos3D);\n"
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

    // Get format information to determine bit depths
    const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(m_inputFormat);
    const VkMpFormatInfo* outputMpInfo = YcbcrVkFormatInfo(m_outputFormat);

    // Determine bit depth from the formats
    const uint32_t inputBitDepth = inputMpInfo ? GetBitsPerChannel(inputMpInfo->planesLayout) : 8;
    const uint32_t outputBitDepth = outputMpInfo ? GetBitsPerChannel(outputMpInfo->planesLayout) : 8;

    // Determine if we're using limited or full range for input and output
    // Default to limited range as it's more common for YCbCr content
    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo =
        m_samplerYcbcrConversion.GetSamplerYcbcrConversionCreateInfo();
    const bool isInputLimitedRange = (samplerYcbcrConversionCreateInfo.ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
    const bool isOutputLimitedRange = isInputLimitedRange; // Usually same as input, but could be configurable

    // Check if input or output are buffers
    const bool isInputBuffer = m_inputIsBuffer;
    const bool isOutputBuffer = m_outputIsBuffer;

    // Check if we need to do any bit depth conversion
    const bool needsBitDepthConversion = (inputBitDepth != outputBitDepth);

    // Check if we need to do any range conversion
    const bool needsRangeConversion = (isInputLimitedRange != isOutputLimitedRange);

    std::stringstream shaderStr;

    // 1. Generate header and push constants
    GenHeaderAndPushConst(shaderStr);

    // 2. Generate IO bindings
    // Input Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   true,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Output Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true,  // imageArray
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    shaderStr << "\n\n";

    // Determine input and output plane configurations
    const bool hasInputChroma = (m_inputImageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0;
    const bool hasOutputChroma = (m_outputImageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0;

    // Determine if input is two-plane (e.g., NV12) or three-plane (e.g., I420)
    const bool isInputTwoPlane = (m_inputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) &&
                          !(m_inputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT);

    // Determine if output is two-plane (e.g., NV12) or three-plane (e.g., I420)
    const bool isOutputTwoPlane = (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) &&
                           !(m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT);

    // 3. Add any bit depth handling functions needed
    if (isInputBuffer && inputBitDepth > 8) {
        bool isMSB = true; // Default to MSB-aligned (most common case)
        GenExtractHighBitDepthFunc(shaderStr, isMSB, inputBitDepth);
    }

    // 4. Add buffer read/write functions if needed
    if (isInputBuffer) {
        // Add fetch functions for Y and CbCr from buffer
        GenFetchYFromBufferFunc(shaderStr, inputBitDepth > 8, inputBitDepth);
        GenFetchCbCrFromBufferFunc(shaderStr, inputBitDepth > 8, inputBitDepth);
    }

    // 5. Add YCbCr normalization and denormalization functions for bit depth conversion
    if (needsBitDepthConversion || needsRangeConversion) {
        // Generate normalization functions for input format
        GenYCbCrNormalizationFuncs(shaderStr, inputBitDepth, isInputLimitedRange, hasInputChroma);

        // Generate denormalization functions for output format
        GenYCbCrDeNormalizationFuncs(shaderStr, outputBitDepth, isOutputLimitedRange, hasOutputChroma);
    }

    // 6. Generate the read function for YCbCr data
    GenReadYCbCrBuffer(shaderStr, isInputBuffer, inputBitDepth, isInputTwoPlane, m_inputEnableMsbToLsbShift, m_inputImageAspects);

    // 7. Generate the write function for YCbCr data
    GenWriteYCbCrBuffer(shaderStr, isOutputBuffer, outputBitDepth, isOutputTwoPlane, m_outputEnableLsbToMsbShift, m_outputImageAspects);

    // 8. Helper function for combined normalization and denormalization
    if (needsBitDepthConversion || needsRangeConversion) {
        GenConvertYCbCrFormat(shaderStr, inputBitDepth, outputBitDepth, isInputLimitedRange, isOutputLimitedRange);
    }

    // 9. Main function
    shaderStr <<
        "void main()\n"
        "{\n";

    // 10. Handle position calculation
    if (isInputBuffer || isOutputBuffer) {
        // Use buffer position calculation
        GenHandleBufferPosition(shaderStr);
    } else {
        // Use image position calculation
        GenHandleImagePosition(shaderStr);
    }

    // 11. Calculate source position with replication if enabled
    GenHandleSourcePositionWithReplicate(shaderStr, m_enableRowAndColumnReplication);

    // 12. Handle YCbCr processing

    // For inputs with chroma, we need to handle subsampling
    // Get subsampling ratios for input format
    const uint32_t chromaHorzRatio = (inputMpInfo != nullptr) ? (1 << inputMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t chromaVertRatio = (inputMpInfo != nullptr) ? (1 << inputMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1;

    // Generate condition for chroma processing based on actual subsampling
    shaderStr <<
        "    // Handle proper subsampling based on format (" <<
        (chromaHorzRatio == 2 ? (chromaVertRatio == 2 ? "4:2:0" : "4:2:2") : "4:4:4") << ")\n";

    // Generate the chroma position condition with a boolean variable
    GenHandleChromaPosition(shaderStr, chromaHorzRatio, chromaVertRatio, true, "pos", "processChroma");

    // Initialize chroma positions with default values
    shaderStr << "    // Initialize chroma positions\n"
              << "    ivec2 chromaSrcPos = srcPos;\n"
              << "    ivec2 chromaPos = pos;\n\n"
              << "    // Check if we need to process chroma\n"
              << "    if (processChroma) {\n";

    // Generate chroma position calculations for source position
    GenCalculateChromaPosition(shaderStr, chromaHorzRatio, chromaVertRatio, "srcPos", "chromaSrcPos", 8);

    // Generate chroma position calculations for destination position
    GenCalculateChromaPosition(shaderStr, chromaHorzRatio, chromaVertRatio, "pos", "chromaPos", 8);

    shaderStr << "    }\n";

    // Read YCbCr data using the helper function
    shaderStr << "\n"
              << "    // Read YCbCr data from source\n"
              << "    vec3 YCbCrRawIn = readYCbCrFromSource(srcPos, chromaSrcPos, pushConstants.srcLayer, processChroma);\n\n";

    // Process the data based on whether we need conversion
    if (needsBitDepthConversion || needsRangeConversion) {
        shaderStr <<
                 "    // Need format conversion - normalize and denormalize\n"
                 "    vec3 YCbCrRawOut = convertYCbCrFormat(YCbCrRawIn);\n\n";
    } else {
        shaderStr <<
                 "    // No format conversion needed - direct copy\n"
                 "    vec3 YCbCrRawOut = YCbCrRawIn;\n\n";
    }

    // Write the processed data using the helper function
    shaderStr <<
                 "    // Write processed data to destination\n"
                 "    writeYCbCrToDestination(YCbCrRawOut, pos, chromaPos, pushConstants.dstLayer, processChroma);\n"
                 "\n\n";

    // Close the main function
    shaderStr << "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
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

    // 1. Generate header and push constants
    GenHeaderAndPushConst(shaderStr);

    // 2. Generate output image bindings
    shaderStr << " // The output descriptors binding\n";
    // Output Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true,  // imageArray
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    shaderStr << "\n\n";

    // Get format information to determine subsampling ratios
    const VkMpFormatInfo* outputMpInfo = YcbcrVkFormatInfo(m_outputFormat);
    // Get subsampling ratios for output format
    const uint32_t chromaHorzRatio = (outputMpInfo != nullptr) ? (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t chromaVertRatio = (outputMpInfo != nullptr) ? (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1;


    // 3. Main function
    shaderStr <<
        "void main()\n"
        "{\n";

    // 4. Handle position calculation
    GenHandleImagePosition(shaderStr);

    // 5. Clear operations for Y plane
    shaderStr <<
        "    // Clear Y plane with 50% intensity\n"
        "    imageStore(outputImageY, ivec3(pos, pushConstants.dstLayer), vec4(0.5, 0, 0, 1));\n"
        "\n";

    // Handle CbCr plane clearing based on format's subsampling
    shaderStr <<
        "    // Clear CbCr plane with " <<
        (chromaHorzRatio == 2 ? (chromaVertRatio == 2 ? "4:2:0" : "4:2:2") : "4:4:4") <<
        " subsampling\n";

    // Generate a boolean to track whether this position needs chroma clearing
    GenHandleChromaPosition(shaderStr, chromaHorzRatio, chromaVertRatio, true, "pos", "shouldClearChroma");

    // Handle position for chroma planes
    shaderStr << "    ivec2 chromaPos = pos;\n";
    shaderStr << "    if (shouldClearChroma) {\n";

    // Calculate chroma position if necessary
    GenCalculateChromaPosition(shaderStr, chromaHorzRatio, chromaVertRatio, "pos", "chromaPos", 8);

    // For 2-plane format, output CbCr together
    if (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
        shaderStr << "        // Clear CbCr plane with 50% intensity (middle range)\n"
                  << "        imageStore(outputImageCbCr, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0.5, 0.0, 1.0));\n";
    }

    // For 3-plane format, handle Cb and Cr separately
    if (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
        shaderStr << "        // Clear separate Cb and Cr planes with 50% intensity (middle range)\n"
                  << "        imageStore(outputImageCb, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0.0, 0.0, 1.0));\n"
                  << "        imageStore(outputImageCr, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0.0, 0.0, 1.0));\n";
    }

    shaderStr << "    }\n"
              << "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        std::cout << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

uint32_t VulkanFilterYuvCompute::GetPlaneIndex(VkImageAspectFlagBits planeAspect) {

    // Returns index 0 for VK_IMAGE_ASPECT_COLOR_BIT and VK_IMAGE_ASPECT_PLANE_0_BIT
    // Returns index 1 for VK_IMAGE_ASPECT_PLANE_1_BIT
    // Returns index 2 for VK_IMAGE_ASPECT_PLANE_2_BIT

    // First, verify it's a plane aspect bit
    assert(planeAspect & validAspects);

    if (planeAspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        return 0;
    }

    // Alternatively, without intrinsics:
    return (planeAspect & VK_IMAGE_ASPECT_PLANE_0_BIT) ? 0 :
           (planeAspect & VK_IMAGE_ASPECT_PLANE_1_BIT) ? 1 : 2;
}

uint32_t VulkanFilterYuvCompute::UpdateBufferDescriptorSets(
                                    const VkBuffer*            vkBuffers,
                                    uint32_t                   numVkBuffers,
                                    const VkSubresourceLayout* vkBufferSubresourceLayout,
                                    uint32_t                   numPlanes,
                                    VkImageAspectFlags         validImageAspects,
                                    uint32_t&                  descrIndex,
                                    uint32_t&                  baseBinding,
                                    VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                    VkDescriptorBufferInfo     bufferDescriptors[maxNumComputeDescr],
                                    std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                    const uint32_t maxDescriptors)
{

    validImageAspects &= validAspects;
    uint32_t curImageAspect = 0;
    uint32_t bufferIndex = 0;
    while(validImageAspects) {

        if (validImageAspects & (VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect) ) {

            uint32_t planeNum = GetPlaneIndex((VkImageAspectFlagBits)(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect));
            uint32_t dstBinding = baseBinding;
            if (curImageAspect > 0) {
                // the first plane is 1, second plane is 2, the 3rd is 3
                dstBinding += (1 + planeNum);
            }

            writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
            writeDescriptorSets[descrIndex].dstBinding = dstBinding;
            writeDescriptorSets[descrIndex].descriptorCount = 1;
            writeDescriptorSets[descrIndex].descriptorType = descriptorType;

            bufferDescriptors[descrIndex].buffer = vkBuffers[bufferIndex];
            bufferDescriptors[descrIndex].offset = vkBufferSubresourceLayout[planeNum].offset;
            bufferDescriptors[descrIndex].range =  vkBufferSubresourceLayout[planeNum].arrayPitch;
            writeDescriptorSets[descrIndex].pBufferInfo = &bufferDescriptors[descrIndex];
            descrIndex++;
            validImageAspects &= ~(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect);
            bufferIndex = std::min(numVkBuffers - 1, bufferIndex + 1);
        }

        curImageAspect++;
    }
    assert(descrIndex <= maxDescriptors);
    return descrIndex;
}

uint32_t VulkanFilterYuvCompute::UpdateImageDescriptorSets(
                                    const VkImageResourceView* imageView,
                                    VkImageAspectFlags         validImageAspects,
                                    VkSampler                  convSampler,
                                    VkImageLayout              imageLayout,
                                    uint32_t&                  descrIndex,
                                    uint32_t&                  baseBinding,
                                    VkDescriptorType           descriptorType, // Ex: VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                    VkDescriptorImageInfo      imageDescriptors[maxNumComputeDescr],
                                    std::array<VkWriteDescriptorSet, maxNumComputeDescr>& writeDescriptorSets,
                                    const uint32_t maxDescriptors)
{

    validImageAspects &= validAspects;
    uint32_t curImageAspect = 0;
    const uint32_t numPlanes = imageView->GetNumberOfPlanes();
    while(validImageAspects) {

        if (validImageAspects & (VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect) ) {

            VkSampler ccSampler = (curImageAspect == 0) ? convSampler : VK_NULL_HANDLE;
            uint32_t planeNum = GetPlaneIndex((VkImageAspectFlagBits)(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect));
            assert(planeNum < numPlanes);
            uint32_t dstBinding = baseBinding;
            if (curImageAspect > 0) {
                // the first plane is 1, second plane is 2, the 3rd is 3
                dstBinding += (1 + planeNum);
            }

            writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
            writeDescriptorSets[descrIndex].dstBinding = dstBinding;
            writeDescriptorSets[descrIndex].descriptorCount = 1;
            writeDescriptorSets[descrIndex].descriptorType = (ccSampler != VK_NULL_HANDLE) ?
                                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                                              descriptorType;
            imageDescriptors[descrIndex].sampler = ccSampler;
            imageDescriptors[descrIndex].imageView = (curImageAspect == 0) ?
                                                      imageView->GetImageView() :
                                                      imageView->GetPlaneImageView(planeNum);
            assert(imageDescriptors[descrIndex].imageView);
            imageDescriptors[descrIndex].imageLayout = imageLayout;
            writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // Y (0) plane
            descrIndex++;
            validImageAspects &= ~(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect);
        }

        curImageAspect++;
    }
    assert(descrIndex <= maxDescriptors);
    return descrIndex;
}

// Image input -> Image output
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     const VkImageResourceView* inImageView,
                                                     const VkVideoPictureResourceInfoKHR * inImageResourceInfo,
                                                     const VkImageResourceView* outImageView,
                                                     const VkVideoPictureResourceInfoKHR * outImageResourceInfo,
                                                     uint32_t bufferIdx)
{

    assert(cmdBuf != VK_NULL_HANDLE);

    m_vkDevCtx->CmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline.getPipeline());

    VkDescriptorSetLayoutCreateFlags layoutMode = m_descriptorSetLayout.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode();

    switch (layoutMode) {
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR:
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT:
        {

            VkDescriptorImageInfo imageDescriptors[maxNumComputeDescr]{};
            std::array<VkWriteDescriptorSet, maxNumComputeDescr> writeDescriptorSets{};

            // Images
            uint32_t set = 0;
            uint32_t descrIndex = 0;
            uint32_t dstBinding = 0;

            // IN 0: RGBA color converted by an YCbCr sample
            // IN 1: y plane - G -> R8
            // IN 2: Cb or Cr or CbCr plane - BR -> R8B8
            // IN 3: Cr or Cb plane - R -> R8
            UpdateImageDescriptorSets(inImageView,
                                      m_inputImageAspects,
                                      m_samplerYcbcrConversion.GetSampler(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      descrIndex,
                                      dstBinding,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      imageDescriptors,
                                      writeDescriptorSets,
                                      maxNumComputeDescr / 2 /* max descriptors */);

            dstBinding = 4;
            // OUT 4: Out RGBA or single planar YCbCr image
            // OUT 5: y plane - G -> R8
            // OUT 6: Cb or Cr or CbCr plane - BR -> R8B8
            // OUT 7: Cr or Cb plane - R -> R8
            UpdateImageDescriptorSets(outImageView,
                                      m_outputImageAspects,
                                      VK_NULL_HANDLE,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      descrIndex,
                                      dstBinding,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      imageDescriptors,
                                      writeDescriptorSets,
                                      maxNumComputeDescr /* max descriptors */);

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

    struct ivec2 {
        uint32_t width;
        uint32_t height;

        ivec2() : width(0), height(0) {}
        ivec2(int32_t width_, int32_t height_) : width(width_), height(height_) {}
    };

    struct PushConstants {
        uint32_t srcLayer;
        uint32_t dstLayer;
        ivec2    inputSize;
        ivec2    outputSize;
        uint32_t yOffset;   // Y plane offset
        uint32_t cbOffset;  // Cb plane offset
        uint32_t crOffset;  // Cr plane offset
        uint32_t yPitch;    // Y plane pitch
        uint32_t cbPitch;   // Cb plane pitch
        uint32_t crPitch;   // Cr plane pitch
    };

    PushConstants pushConstants = {
            inImageResourceInfo->baseArrayLayer, // Set the source layer index
            outImageResourceInfo->baseArrayLayer, // Set the destination layer index
            ivec2(inImageResourceInfo->codedExtent.width, inImageResourceInfo->codedExtent.height),
            ivec2(outImageResourceInfo->codedExtent.width, outImageResourceInfo->codedExtent.height),
            0,  // yOffset - not used for image input
            0,  // cbOffset - not used for image input
            0,  // crOffset - not used for image input
            0,  // yPitch - not used for image input
            0,  // cbPitch - not used for image input
            0   // crPitch - not used for image input
    };

    m_vkDevCtx->CmdPushConstants(cmdBuf,
                                 m_descriptorSetLayout.GetPipelineLayout(),
                                 VK_SHADER_STAGE_COMPUTE_BIT,
                                 0,
                                 sizeof(PushConstants),
                                 &pushConstants);

    const uint32_t  workgroupWidth  = (pushConstants.outputSize.width  + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t  workgroupHeight = (pushConstants.outputSize.height + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    return VK_SUCCESS;
}

// Buffer input -> Image output
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     const VkBuffer*            inBuffers,
                                                     uint32_t                   numInBuffers,
                                                     const VkFormat*            inBufferFormats,
                                                     const VkSubresourceLayout* inBufferSubresourceLayouts,
                                                     uint32_t                   inBufferNumPlanes,
                                                     const VkImageResourceView* outImageView,
                                                     const VkVideoPictureResourceInfoKHR* outImageResourceInfo,
                                                     const VkBufferImageCopy* pBufferImageCopy,
                                                     uint32_t bufferIdx)
{
    assert(cmdBuf != VK_NULL_HANDLE);
    assert(m_inputIsBuffer  == true);
    assert(m_outputIsBuffer == false);

    m_vkDevCtx->CmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline.getPipeline());

    VkDescriptorSetLayoutCreateFlags layoutMode = m_descriptorSetLayout.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode();

    switch (layoutMode) {
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR:
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT:
        {
            VkDescriptorImageInfo imageDescriptors[maxNumComputeDescr / 2]{};
            VkDescriptorBufferInfo bufferDescriptors[maxNumComputeDescr / 2]{};
            std::array<VkWriteDescriptorSet, maxNumComputeDescr> writeDescriptorSets{};

            uint32_t set = 0;
            uint32_t descrIndex = 0;
            uint32_t dstBinding = 0;

            // Buffer input handling
            // IN 0: Single buffer YCbCr
            // IN 1: Y plane buffer
            // IN 2: Cb, Cr or CbCr plane buffer
            // IN 3: Cr plane buffer
            UpdateBufferDescriptorSets(inBuffers, numInBuffers,
                                       inBufferSubresourceLayouts, inBufferNumPlanes,
                                       m_inputImageAspects,
                                       descrIndex, dstBinding,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       bufferDescriptors,
                                       writeDescriptorSets,
                                       maxNumComputeDescr / 2);


            // Image output
            dstBinding = 4;
            // OUT 4: Out RGBA or single planar YCbCr image
            // OUT 5: y plane - G -> R8
            // OUT 6: Cb or Cr or CbCr plane - BR -> R8B8
            // OUT 7: Cr or Cb plane - R -> R8
            UpdateImageDescriptorSets(outImageView,
                                      m_outputImageAspects,
                                      VK_NULL_HANDLE,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      descrIndex,
                                      dstBinding,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      imageDescriptors,
                                      writeDescriptorSets,
                                      maxNumComputeDescr /* max descriptors */);

            assert(descrIndex <= maxNumComputeDescr);
            assert(descrIndex >= 2);

            if (layoutMode == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
                m_vkDevCtx->CmdPushDescriptorSetKHR(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                    m_descriptorSetLayout.GetPipelineLayout(),
                                                    set, descrIndex, writeDescriptorSets.data());
            } else {
                VkDeviceOrHostAddressConstKHR descriptorBufferDeviceAddress =
                      m_descriptorSetLayout.UpdateDescriptorBuffer(bufferIdx,
                                                                   set,
                                                                   descrIndex,
                                                                   writeDescriptorSets.data());


                // Descriptor buffer bindings
                VkDescriptorBufferBindingInfoEXT bindingInfo{};
                bindingInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                bindingInfo.pNext = nullptr;
                bindingInfo.address = descriptorBufferDeviceAddress.deviceAddress;
                bindingInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                                    VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                m_vkDevCtx->CmdBindDescriptorBuffersEXT(cmdBuf, 1, &bindingInfo);

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

    struct ivec2 {
        uint32_t width;
        uint32_t height;

        ivec2() : width(0), height(0) {}
        ivec2(int32_t width_, int32_t height_) : width(width_), height(height_) {}
    };

    struct PushConstants {
        uint32_t srcLayer;
        uint32_t dstLayer;
        ivec2    inputSize;
        ivec2    outputSize;
        uint32_t yOffset;   // Y plane offset
        uint32_t cbOffset;  // Cb plane offset
        uint32_t crOffset;  // Cr plane offset
        uint32_t yPitch;    // Y plane pitch
        uint32_t cbPitch;   // Cb plane pitch
        uint32_t crPitch;   // Cr plane pitch
    };

    uint32_t width, height;
    uint32_t rowPitch;

    assert(pBufferImageCopy);
    width = pBufferImageCopy->bufferRowLength > 0 ?
            pBufferImageCopy->bufferRowLength :
            pBufferImageCopy->imageExtent.width;
    height = pBufferImageCopy->bufferImageHeight > 0 ?
             pBufferImageCopy->bufferImageHeight :
             pBufferImageCopy->imageExtent.height;
    rowPitch = width;

    VkExtent3D outputExtent = outImageView->GetImageResource()->GetImageCreateInfo().extent;

    VkDeviceSize planeSize = width * height;
    VkDeviceSize yOffset = pBufferImageCopy ? pBufferImageCopy->bufferOffset : 0;
    VkDeviceSize cbOffset = yOffset + planeSize;
    VkDeviceSize crOffset = cbOffset + (planeSize / 4);

    PushConstants pushConstants = {
            pBufferImageCopy->imageSubresource.baseArrayLayer,
            outImageResourceInfo->baseArrayLayer,
            ivec2(width, height),
            ivec2(outputExtent.width, outputExtent.height),
            static_cast<uint32_t>(yOffset),
            static_cast<uint32_t>(cbOffset),
            static_cast<uint32_t>(crOffset),
            rowPitch,
            rowPitch / 2,  // For 4:2:0 format
            rowPitch / 2   // For 4:2:0 format
    };

    m_vkDevCtx->CmdPushConstants(cmdBuf,
                                 m_descriptorSetLayout.GetPipelineLayout(),
                                 VK_SHADER_STAGE_COMPUTE_BIT,
                                 0,
                                 sizeof(PushConstants),
                                 &pushConstants);

    const uint32_t workgroupWidth = (pushConstants.outputSize.width + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t workgroupHeight = (pushConstants.outputSize.height + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    return VK_SUCCESS;
}

// Image input -> Buffer output
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     const VkImageResourceView* inImageView,
                                                     const VkVideoPictureResourceInfoKHR* inImageResourceInfo,
                                                     const VkBuffer*            outBuffers,        // with size numOutBuffers
                                                     uint32_t                   numOutBuffers,
                                                     const VkFormat*            outBufferFormats,   // with size outBufferNumPlanes
                                                     const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                                     uint32_t                   outBufferNumPlanes,
                                                     const VkBufferImageCopy*   pBufferImageCopy,
                                                     uint32_t bufferIdx)
{
    assert(cmdBuf != VK_NULL_HANDLE);
    assert(m_inputIsBuffer  == false);
    assert(m_outputIsBuffer == true);

    m_vkDevCtx->CmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline.getPipeline());

    VkDescriptorSetLayoutCreateFlags layoutMode = m_descriptorSetLayout.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode();

    switch (layoutMode) {
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR:
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT:
        {
            VkDescriptorImageInfo imageDescriptors[maxNumComputeDescr / 2]{};
            VkDescriptorBufferInfo bufferDescriptors[maxNumComputeDescr / 2]{};
            std::array<VkWriteDescriptorSet, maxNumComputeDescr> writeDescriptorSets{};

            uint32_t set = 0;
            uint32_t descrIndex = 0;
            uint32_t dstBinding = 0;

            // IN 0: RGBA color converted by an YCbCr sample
            // IN 1: y plane - G -> R8
            // IN 2: Cb or Cr or CbCr plane - BR -> R8B8
            // IN 3: Cr or Cb plane - R -> R8
            UpdateImageDescriptorSets(inImageView,
                                      m_inputImageAspects,
                                      m_samplerYcbcrConversion.GetSampler(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      descrIndex,
                                      dstBinding,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      imageDescriptors,
                                      writeDescriptorSets,
                                      maxNumComputeDescr / 2 /* max descriptors */);

            // Output buffer handling
            dstBinding = 4;
            // OUT 0: Single buffer YCbCr
            // OUT 1: Y plane buffer
            // OUT 2: Cb, Cr or CbCr plane buffer
            // OUT 3: Cr or Cb plane buffer
            UpdateBufferDescriptorSets(outBuffers, numOutBuffers,
                                       outBufferSubresourceLayouts, outBufferNumPlanes,
                                       m_inputImageAspects,
                                       descrIndex, dstBinding,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       bufferDescriptors,
                                       writeDescriptorSets,
                                       maxNumComputeDescr);

            assert(descrIndex <= maxNumComputeDescr);
            assert(descrIndex >= 2);

            if (layoutMode == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
                m_vkDevCtx->CmdPushDescriptorSetKHR(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   m_descriptorSetLayout.GetPipelineLayout(),
                                                   set, descrIndex, writeDescriptorSets.data());
            } else {
                VkDeviceOrHostAddressConstKHR descriptorBufferDeviceAddress =
                     m_descriptorSetLayout.UpdateDescriptorBuffer(bufferIdx,
                                                                 set,
                                                                 descrIndex,
                                                                 writeDescriptorSets.data());

                // Descriptor buffer bindings
                VkDescriptorBufferBindingInfoEXT bindingInfo{};
                bindingInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                bindingInfo.pNext = nullptr;
                bindingInfo.address = descriptorBufferDeviceAddress.deviceAddress;
                bindingInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                                   VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                m_vkDevCtx->CmdBindDescriptorBuffersEXT(cmdBuf, 1, &bindingInfo);

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

    struct ivec2 {
        uint32_t width;
        uint32_t height;

        ivec2() : width(0), height(0) {}
        ivec2(int32_t width_, int32_t height_) : width(width_), height(height_) {}
    };

    struct PushConstants {
        uint32_t srcLayer;
        uint32_t dstLayer;
        ivec2    inputSize;
        ivec2    outputSize;
        uint32_t yOffset;   // Y plane offset
        uint32_t cbOffset;  // Cb plane offset
        uint32_t crOffset;  // Cr plane offset
        uint32_t yPitch;    // Y plane pitch
        uint32_t cbPitch;   // Cb plane pitch
        uint32_t crPitch;   // Cr plane pitch
    };

    uint32_t width, height;
    uint32_t rowPitch;
    VkExtent3D inputExtent = inImageView->GetImageResource()->GetImageCreateInfo().extent;

    if (pBufferImageCopy) {
        width = pBufferImageCopy->bufferRowLength > 0 ?
                pBufferImageCopy->bufferRowLength :
                pBufferImageCopy->imageExtent.width;
        height = pBufferImageCopy->bufferImageHeight > 0 ?
                pBufferImageCopy->bufferImageHeight :
                pBufferImageCopy->imageExtent.height;
        rowPitch = width;
    } else {
        width = inputExtent.width;
        height = inputExtent.height;
        rowPitch = width;
    }

    VkDeviceSize planeSize = width * height;
    VkDeviceSize yOffset = pBufferImageCopy ? pBufferImageCopy->bufferOffset : 0;
    VkDeviceSize cbOffset = yOffset + planeSize;
    VkDeviceSize crOffset = cbOffset + (planeSize / 4);

    PushConstants pushConstants = {
            inImageResourceInfo->baseArrayLayer,
            0, // Destination layer (buffer has no layers)
            ivec2(inputExtent.width, inputExtent.height),
            ivec2(width, height),
            static_cast<uint32_t>(yOffset),
            static_cast<uint32_t>(cbOffset),
            static_cast<uint32_t>(crOffset),
            rowPitch,
            rowPitch / 2,  // For 4:2:0 format
            rowPitch / 2   // For 4:2:0 format
    };

    m_vkDevCtx->CmdPushConstants(cmdBuf,
                               m_descriptorSetLayout.GetPipelineLayout(),
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0,
                               sizeof(PushConstants),
                               &pushConstants);

    const uint32_t workgroupWidth = (pushConstants.outputSize.width + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t workgroupHeight = (pushConstants.outputSize.height + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    return VK_SUCCESS;
}

// Buffer input -> Buffer output (all buffer case)
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     const VkBuffer*            inBuffers,
                                                     uint32_t                   numInBuffers,
                                                     const VkFormat*            inBufferFormats, // with size inBufferNumPlanes
                                                     const VkSubresourceLayout* inBufferSubresourceLayouts,
                                                     uint32_t                   numInPlanes,
                                                     const VkExtent3D&          inBufferExtent,
                                                     const VkBuffer*            outBuffers,
                                                     uint32_t                   numOutBuffers,
                                                     const VkFormat*            outBufferFormats,
                                                     const VkSubresourceLayout* outBufferSubresourceLayouts,
                                                     uint32_t                   numOutPlanes,
                                                     const VkExtent3D&          outBufferExtent,
                                                     uint32_t bufferIdx)
{
    assert(cmdBuf != VK_NULL_HANDLE);
    assert(m_inputIsBuffer  == true);
    assert(m_outputIsBuffer == true);

    m_vkDevCtx->CmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline.getPipeline());

    VkDescriptorSetLayoutCreateFlags layoutMode = m_descriptorSetLayout.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode();

    switch (layoutMode) {
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR:
        case VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT:
        {
            VkDescriptorBufferInfo bufferDescriptors[maxNumComputeDescr]{};
            std::array<VkWriteDescriptorSet, maxNumComputeDescr> writeDescriptorSets{};

            uint32_t set = 0;
            uint32_t descrIndex = 0;
            uint32_t dstBinding = 0;

            // Input buffer handling
            // IN 0: Single buffer YCbCr
            // IN 1: Y plane buffer
            // IN 2: Cb, Cr or CbCr plane buffer
            // IN 3: Cr plane buffer
            UpdateBufferDescriptorSets(inBuffers, numInBuffers,
                                       inBufferSubresourceLayouts, numInPlanes,
                                       m_inputImageAspects,
                                       descrIndex, dstBinding,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       bufferDescriptors,
                                       writeDescriptorSets,
                                       maxNumComputeDescr / 2);

            // Output buffer handling
            dstBinding = 4;
            // OUT 0: Single buffer YCbCr
            // OUT 1: Y plane buffer
            // OUT 2: Cb, Cr or CbCr plane buffer
            // OUT 3: Cr or Cb plane buffer
            UpdateBufferDescriptorSets(outBuffers, numOutBuffers,
                                       outBufferSubresourceLayouts, numOutPlanes,
                                       m_inputImageAspects,
                                       descrIndex, dstBinding,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       bufferDescriptors,
                                       writeDescriptorSets,
                                       maxNumComputeDescr);

            assert(descrIndex <= maxNumComputeDescr);
            assert(descrIndex >= 2);

            if (layoutMode == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
                m_vkDevCtx->CmdPushDescriptorSetKHR(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                  m_descriptorSetLayout.GetPipelineLayout(),
                                                  set, descrIndex, writeDescriptorSets.data());
            } else {
                VkDeviceOrHostAddressConstKHR descriptorBufferDeviceAddress =
                      m_descriptorSetLayout.UpdateDescriptorBuffer(bufferIdx,
                                                                 set,
                                                                 descrIndex,
                                                                 writeDescriptorSets.data());

                // Descriptor buffer bindings
                VkDescriptorBufferBindingInfoEXT bindingInfo{};
                bindingInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                bindingInfo.pNext = nullptr;
                bindingInfo.address = descriptorBufferDeviceAddress.deviceAddress;
                bindingInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                                    VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                m_vkDevCtx->CmdBindDescriptorBuffersEXT(cmdBuf, 1, &bindingInfo);

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

    struct ivec2 {
        uint32_t width;
        uint32_t height;

        ivec2() : width(0), height(0) {}
        ivec2(int32_t width_, int32_t height_) : width(width_), height(height_) {}
    };

    struct PushConstants {
        uint32_t srcLayer;    // src image layer to use
        uint32_t dstLayer;    // dst image layer to use
        ivec2    inputSize;   // input image or buffer extent
        ivec2    outputSize;  // output image or buffer extent
        uint32_t inYOffset;   // input  buffer Y plane offset
        uint32_t inCbOffset;  // input  buffer Cb plane offset
        uint32_t inCrOffset;  // input  buffer Cr plane offset
        uint32_t inYPitch;    // input  buffer Y plane pitch
        uint32_t inCbPitch;   // input  buffer Cb plane pitch
        uint32_t inCrPitch;   // input  buffer Cr plane pitch
        uint32_t outYOffset;   // output buffer Y plane offset
        uint32_t outCbOffset;  // output buffer Cb plane offset
        uint32_t outCrOffset;  // output buffer Cr plane offset
        uint32_t outYPitch;    // output buffer Y plane pitch
        uint32_t outCbPitch;   // output buffer Cb plane pitch
        uint32_t outCrPitch;   // output buffer Cr plane pitch
    };

    // Calculate buffer parameters
    uint32_t rowPitch = inBufferExtent.width;
    VkDeviceSize planeSize = inBufferExtent.width * inBufferExtent.height;
    VkDeviceSize yOffset = 0;
    VkDeviceSize cbOffset = planeSize;
    VkDeviceSize crOffset = cbOffset + (planeSize / 4);

    PushConstants pushConstants = {
            0, // Source layer (buffer has no layers)
            0, // Destination layer (buffer has no layers)
            ivec2(inBufferExtent.width, inBufferExtent.height),
            ivec2(outBufferExtent.width, outBufferExtent.height),
            static_cast<uint32_t>(yOffset),
            static_cast<uint32_t>(cbOffset),
            static_cast<uint32_t>(crOffset),
            rowPitch,
            rowPitch / 2,  // For 4:2:0 format
            rowPitch / 2   // For 4:2:0 format
    };

    m_vkDevCtx->CmdPushConstants(cmdBuf,
                               m_descriptorSetLayout.GetPipelineLayout(),
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0,
                               sizeof(PushConstants),
                               &pushConstants);

    const uint32_t workgroupWidth = (pushConstants.outputSize.width + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t workgroupHeight = (pushConstants.outputSize.height + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    return VK_SUCCESS;
}
