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
#include "VkCodecUtils/VkEncoderStdioLatch.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

#include <cstdlib>
static bool dumpShaders = (getenv("VK_FILTER_DUMP_SHADERS") != nullptr);

// =============================================================================
// TransferResource factory method implementations
// =============================================================================

TransferResource TransferResource::fromBufferResource(const VkBufferResource& bufRes,
                                                      VkFormat fmt,
                                                      uint32_t width, uint32_t height,
                                                      uint32_t planes,
                                                      const VkDeviceSize* planeOffsets,
                                                      const VkDeviceSize* planeRowPitches) {
    TransferResource r{};
    r.type = Type::BUFFER;
    r.buffer = bufRes.GetBuffer();
    r.bufferTotalSize = bufRes.GetMaxSize();
    r.format = fmt;
    r.extent = {width, height, 1};
    
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(fmt);
    
    // Determine number of planes from format if not specified
    if (planes == 0 || planes > kMaxPlanes) {
        if (mpInfo && mpInfo->planesLayout.numberOfExtraPlanes > 0) {
            planes = mpInfo->planesLayout.numberOfExtraPlanes + 1;
        } else {
            planes = 1;
        }
    }
    r.numPlanes = planes;
    
    // Calculate geometry for each plane
    VkDeviceSize autoOffset = 0;
    
    for (uint32_t plane = 0; plane < planes; ++plane) {
        PlaneBufferGeometry& geom = r.planeGeometry[plane];
        
        // Calculate subsampled dimensions for this plane
        uint32_t planeWidth = width;
        uint32_t planeHeight = height;
        uint32_t bytesPerPixel = 1;
        
        if (mpInfo) {
            if (plane > 0) {
                // Secondary planes may be subsampled
                uint32_t subsampledX = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledX;
                uint32_t subsampledY = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledY;
                planeWidth = (width + subsampledX - 1) / subsampledX;
                planeHeight = (height + subsampledY - 1) / subsampledY;
            }
            
            // Determine bytes per pixel for this plane's format
            VkFormat planeFormat = mpInfo->vkPlaneFormat[plane];
            switch (planeFormat) {
                case VK_FORMAT_R8_UNORM:
                    bytesPerPixel = 1;
                    break;
                case VK_FORMAT_R8G8_UNORM:
                    bytesPerPixel = 2;
                    break;
                case VK_FORMAT_R16_UNORM:
                    bytesPerPixel = 2;
                    break;
                case VK_FORMAT_R16G16_UNORM:
                    bytesPerPixel = 4;
                    break;
                case VK_FORMAT_R8G8B8_UNORM:
                    bytesPerPixel = 3;
                    break;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_UNORM:
                    bytesPerPixel = 4;
                    break;
                default:
                    // Fallback: estimate from format
                    bytesPerPixel = (mpInfo->planesLayout.bpp + 7) / 8;
                    break;
            }
        } else {
            // Non-YCbCr format - estimate from common formats
            switch (fmt) {
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    bytesPerPixel = 4;
                    break;
                case VK_FORMAT_R8G8B8_UNORM:
                    bytesPerPixel = 3;
                    break;
                case VK_FORMAT_R8_UNORM:
                    bytesPerPixel = 1;
                    break;
                case VK_FORMAT_R16_UNORM:
                    bytesPerPixel = 2;
                    break;
                default:
                    bytesPerPixel = 4;  // Default assumption
                    break;
            }
        }
        
        geom.width = planeWidth;
        geom.height = planeHeight;
        geom.bytesPerPixel = bytesPerPixel;
        
        // Use explicit offsets if provided, otherwise auto-calculate
        if (planeOffsets) {
            geom.offset = planeOffsets[plane];
        } else {
            geom.offset = autoOffset;
        }
        
        // Use explicit row pitches if provided, otherwise tightly packed
        if (planeRowPitches && planeRowPitches[plane] > 0) {
            geom.rowPitch = planeRowPitches[plane];
        } else {
            geom.rowPitch = planeWidth * bytesPerPixel;  // Tightly packed
        }
        
        // Calculate plane size and update auto-offset for next plane
        geom.size = geom.rowPitch * planeHeight;
        autoOffset = geom.offset + geom.size;
    }
    
    return r;
}

static uint32_t RgbaBufferWordsPerPixel(VkFormat format);

VkResult VulkanFilterYuvCompute::Create(const VulkanDeviceContext* vkDevCtx,
                                        uint32_t queueFamilyIndex,
                                        uint32_t queueIndex,
                                        FilterType flterType,
                                        uint32_t maxNumFrames,
                                        VkFormat inputFormat,
                                        VkFormat outputFormat,
                                        uint32_t filterFlags,
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
                                                                                         filterFlags,
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

// Packed (single-plane) YCbCr surfaces carried on an RGBA-typed VkFormat.
//
// Deliberately NOT added to YcbcrVkFormatInfo(): (a) that table is dense-indexed by
// (format - BEGIN_RANGE) with a size assert, so enums 37/64 cannot live in it, and
// (b) the same enum is legitimately plain RGBA elsewhere -- GetOutputFormat() can return
// VK_FORMAT_R8G8B8A8_UNORM as the YCBCR2RGBA *output*. So "is this YCbCr?" is answered by
// the FILTER TYPE, not by the enum; this table only answers "if it is, what is its layout?".
//
// Channel indices are into the vec4 returned by imageLoad: 0=r 1=g 2=b 3=a.
// The packed 4:4:4 descriptor lives in nvidia_utils/vulkan/ycbcrvkinfo.h so that the
// filter, the encoder's staging copy and the format-selection code all read ONE table.
// A hand-written per-format switch here is what transposes Cb and Cr on one format
// while leaving its siblings correct, which is why the channel order is looked up
// rather than spelled out.
typedef VkPackedYcbcrFormatDesc VkPackedYcbcrFormatInfo;

static const VkPackedYcbcrFormatInfo* PackedYcbcrFormatInfo(VkFormat format)
{
    return PackedYcbcrFormatDesc(format);
}

// GLSL component selector for a channel index.
static const char* PackedChan(uint8_t idx)
{
    static const char* const c[4] = { "r", "g", "b", "a" };
    return c[idx & 3];
}

// Is THIS side of THIS filter a YCbCr side? Only then may an RGBA-typed format be
// reinterpreted as packed YCbCr. Without the filter-type check, a genuine RGBA surface
// would be silently treated as AYUV (and vice versa) -- the two are indistinguishable
// from the VkFormat alone.
static const VkPackedYcbcrFormatInfo*
PackedYcbcrSideInfo(VulkanFilterYuvCompute::FilterType filterType, bool isInput, VkFormat format)
{
    if (YcbcrVkFormatInfo(format) != nullptr) {
        return nullptr;   // a real multi-planar YCbCr format; not an alias
    }
    switch (filterType) {
        case VulkanFilterYuvCompute::RGBA2YCBCR: if (isInput)  return nullptr; break;
        case VulkanFilterYuvCompute::YCBCR2RGBA: if (!isInput) return nullptr; break;
        case VulkanFilterYuvCompute::YCBCRCOPY:
        case VulkanFilterYuvCompute::YCBCRCLEAR: break;   // both sides are YCbCr
        default: return nullptr;
    }
    return PackedYcbcrFormatInfo(format);
}

static const char* GlslImageFormatQualifier(VkFormat format);

VkResult VulkanFilterYuvCompute::Init(const VkSamplerYcbcrConversionCreateInfo* pYcbcrConversionCreateInfo,
                                      const VkSamplerCreateInfo* pSamplerCreateInfo)
{
    // Decide ONCE, before anything is generated, whether either side is a packed YCbCr
    // surface on an RGBA-typed enum. Both the shader declaration generator and every
    // reference generator read these members, which is what keeps a declaration and its
    // uses in step -- two sites answering the question independently is how the shader
    // ends up referencing an identifier the declaration generator never emitted.
    m_inputPackedYcbcr  = PackedYcbcrSideInfo(m_filterType, true,  m_inputFormat);
    m_outputPackedYcbcr = PackedYcbcrSideInfo(m_filterType, false, m_outputFormat);

    // Default the luma block to the output's chroma ratio, which is what every
    // RecordCommandBuffer() dispatch already assumed. Each per-type Init may override it
    // (InitYCBCRCOPY forces 2x2 for Adaptive Quantization); the invariant that matters is
    // that the shader and the dispatch grid read the SAME value.
    {
        const VkMpFormatInfo* outMpInfo = YcbcrVkFormatInfo(m_outputFormat);
        m_blockHorzRatio = (outMpInfo != nullptr) ? (1u << outMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1u;
        m_blockVertRatio = (outMpInfo != nullptr) ? (1u << outMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1u;
    }

    // A buffer input or output (FLAG_INPUT_BUFFER, FLAG_OUTPUT_BUFFER): an RGBA buffer on the RGBA side of
    // RGBA2YCBCR or YCBCR2RGBA, a multi-planar YCbCr buffer on the YCbCr side and on either side of YCBCRCOPY,
    // which the device stores in 8- or 16-bit storage buffer elements (GenBufferAccessFuncs). The other filter
    // types, the subsampled luma output and the bit shifts have no buffer access.
    if (m_inputIsBuffer || m_outputIsBuffer) {
        auto checkYcbcrBuffer = [this](VkFormat format) -> VkResult {
            const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
            if (mpInfo == nullptr) {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            const bool sixteenBit = (GetBitsPerChannel(mpInfo->planesLayout) > 8);
            const bool hasStorage = sixteenBit ? m_vkDevCtx->HasStorageBuffer16BitAccess()
                                               : m_vkDevCtx->HasStorageBuffer8BitAccess();
            return hasStorage ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
        };
        VkResult bufferResult = VK_SUCCESS;
        if ((m_filterType == RGBA2YCBCR) || (m_filterType == YCBCR2RGBA)) {
            const bool rgbaInput = (m_filterType == RGBA2YCBCR);
            if (m_inputIsBuffer) {
                bufferResult = rgbaInput ? ((RgbaBufferWordsPerPixel(m_inputFormat) != 0) ? VK_SUCCESS
                                                                                          : VK_ERROR_FORMAT_NOT_SUPPORTED)
                                         : checkYcbcrBuffer(m_inputFormat);
            }
            if ((bufferResult == VK_SUCCESS) && m_outputIsBuffer) {
                bufferResult = rgbaInput ? checkYcbcrBuffer(m_outputFormat)
                                         : ((RgbaBufferWordsPerPixel(m_outputFormat) != 0) ? VK_SUCCESS
                                                                                           : VK_ERROR_FORMAT_NOT_SUPPORTED);
            }
        } else if (m_filterType == YCBCRCOPY) {
            if (m_inputIsBuffer) {
                bufferResult = checkYcbcrBuffer(m_inputFormat);
            }
            if ((bufferResult == VK_SUCCESS) && m_outputIsBuffer) {
                bufferResult = checkYcbcrBuffer(m_outputFormat);
            }
        } else {
            bufferResult = VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if ((bufferResult == VK_SUCCESS) &&
            (m_enableYSubsampling || m_inputEnableMsbToLsbShift || m_outputEnableLsbToMsbShift)) {
            bufferResult = VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (bufferResult != VK_SUCCESS) {
            return bufferResult;
        }
    }

    VkResult result = Configure( m_vkDevCtx,
                                 m_maxNumFrames, // numPoolNodes
                                 m_vkDevCtx->GetComputeQueueFamilyIdx(), // queueFamilyIndex
                                 false,    // createQueryPool - not needed for the compute filter
                                 nullptr,  // pVideoProfile   - not needed for the compute filter
                                 true,     // createSemaphores
                                 true      // createFences
                                );
    if (result != VK_SUCCESS) {
        return result;
    }

    // The shaders read every input with imageLoad(), a multi-planar one plane by plane, so the filter
    // samples nothing: it keeps the conversion's format, model and range for the color matrix and makes
    // no VkSamplerYcbcrConversion, which a format with no chroma sample location cannot have
    // (VUID-VkSamplerYcbcrConversionCreateInfo-format-01650). pSamplerCreateInfo is not read.
    (void)pSamplerCreateInfo;
    if (pYcbcrConversionCreateInfo) {
        m_ycbcrConversionCreateInfo = *pYcbcrConversionCreateInfo;
        m_ycbcrConversionCreateInfo.pNext = nullptr;
    }

    assert(m_queue != VK_NULL_HANDLE);

    // A storage image declared without a format (GlslImageFormatQualifier): the device must read and
    // write those. VulkanDeviceContext enables every core feature the device has.
    const bool readsWithoutFormat = (m_filterType == RGBA2YCBCR) && !m_inputIsBuffer &&
                                    (GlslImageFormatQualifier(m_inputFormat)[0] == '\0');
    const bool writesWithoutFormat = (m_filterType == YCBCR2RGBA) && !m_outputIsBuffer &&
                                     (GlslImageFormatQualifier(m_outputFormat)[0] == '\0');
    if (readsWithoutFormat || writesWithoutFormat) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        m_vkDevCtx->GetPhysicalDeviceFeatures2(m_vkDevCtx->getPhysicalDevice(), &features);
        if ((readsWithoutFormat && !features.features.shaderStorageImageReadWithoutFormat) ||
            (writesWithoutFormat && !features.features.shaderStorageImageWriteWithoutFormat)) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
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
         computeShaderSize = InitRGBA2YCBCR(computeShader);
         break;
     default:
         assert(!"Invalid filter type");
         break;
    }

    // After the shader: the generator decides which input bindings the shader reads.
    result = InitDescriptorSetLayout(m_maxNumFrames);
    if (result != VK_SUCCESS) {
        assert(!"ERROR: InitDescriptorSetLayout!");
        return result;
    }

    return m_computePipeline.CreatePipeline(m_vkDevCtx, m_vulkanShaderCompiler,
                                            computeShader.c_str(), computeShaderSize,
                                            "main",
                                            m_workgroupSizeX, m_workgroupSizeY,
                                            &m_descriptorSetLayout);
}

VkResult VulkanFilterYuvCompute::InitDescriptorSetLayout(uint32_t maxNumFrames)
{
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
        // Binding 0: the input read as one image, an RGBA input or a packed 4:4:4 YCbCr one (AYUV, Y410),
        // for which the shader generator leaves VK_IMAGE_ASPECT_COLOR_BIT in m_inputImageAspects. A
        // multi-planar input is read plane by plane from bindings 1 to 3, and has no binding 0.
        if ((m_inputImageAspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
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

    // Binding 9: Output image (write) for 2x2 subsampled Y (for AQ operations)
    setLayoutBindings.push_back(VkDescriptorSetLayoutBinding{ 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});

    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; // Stage the push constant is for
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants); // Size matches the actual PushConstants struct

    // autoSelect (last argument): the flags argument below is only a preference.
    // CreateDescriptorSet picks push descriptors when VK_KHR_push_descriptor is
    // enabled, descriptor buffers when the device uses them
    // (VulkanDeviceContext::UsesDescriptorBuffers), and plain descriptor sets
    // otherwise.
    // RecordCommandBuffer pushes the descriptors or writes them to the frame's
    // part of the descriptor buffer (vk_filter_test --descriptor-buffer).
    //
    // The filter does not work on a device that enables neither extension. The
    // third arm is a stub -- nothing in this file ever calls
    // VulkanDescriptorSetLayout::WriteDescriptorSet, and the pool it allocates
    // is typed COMBINED_IMAGE_SAMPLER while these bindings are storage
    // images/buffers -- so it fails at Create with VK_ERROR_OUT_OF_POOL_MEMORY,
    // a failure the encoder treats as fatal. A caller-supplied VkDevice lands
    // there too: VulkanDeviceContext's imported path never populates
    // m_reqDeviceExtensions, so FindRequiredDeviceExtension answers "no" for
    // every extension regardless of what the embedder enabled.
    return m_descriptorSetLayout.CreateDescriptorSet(m_vkDevCtx,
                                                     setLayoutBindings,
                                                     VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                     1, &pushConstantRange,
                                                     nullptr, // no sampler Y'CbCr conversion
                                                     maxNumFrames,
                                                     true);
}

static YcbcrBtStandard GetYcbcrPrimariesConstantsId(VkSamplerYcbcrModelConversion modelConversion)
{
    switch (modelConversion) {
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709:
        return YcbcrBtStandardBt709;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:
        return YcbcrBtStandardBt601Ebu;
    case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
        return YcbcrBtStandardBt2020;  // Fixed: was incorrectly returning BT.709
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
              << "    uint srcLayer;         // src image layer to use\n"
              << "    uint dstLayer;         // dst image layer to use\n"
              << "    uint inputWidth;       // input image or buffer width\n"
              << "    uint inputHeight;      // input image or buffer height\n"
              << "    uint outputWidth;      // output image or buffer width\n"
              << "    uint outputHeight;     // output image or buffer height\n"
              << "    uint halfInputWidth;   // (inputWidth + 1) / 2 - precomputed\n"
              << "    uint halfInputHeight;  // (inputHeight + 1) / 2 - precomputed\n"
              << "    uint halfOutputWidth;  // (outputWidth + 1) / 2 - precomputed\n"
              << "    uint halfOutputHeight; // (outputHeight + 1) / 2 - precomputed\n"
              << "    uint inYOffset;        // input  buffer Y plane offset\n"
              << "    uint inCbOffset;       // input  buffer Cb plane offset\n"
              << "    uint inCrOffset;       // input  buffer Cr plane offset\n"
              << "    uint inYPitch;         // input  buffer Y plane pitch\n"
              << "    uint inCbPitch;        // input  buffer Cb plane pitch\n"
              << "    uint inCrPitch;        // input  buffer Cr plane pitch\n"
              << "    uint outYOffset;       // output buffer Y plane offset\n"
              << "    uint outCbOffset;      // output buffer Cb plane offset\n"
              << "    uint outCrOffset;      // output buffer Cr plane offset\n"
              << "    uint outYPitch;        // output buffer Y plane pitch\n"
              << "    uint outCbPitch;       // output buffer Cb plane pitch\n"
              << "    uint outCrPitch;       // output buffer Cr plane pitch\n"
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
static void GenHeaderAndPushConst(std::stringstream& shaderStr, bool readsWithoutFormat = false,
                                  bool usesBuffers = false)
{
    shaderStr << "#version 450\n";
    if (readsWithoutFormat) {
        // An image declared without a format can be read only with this extension (and the device's
        // shaderStorageImageReadWithoutFormat).
        shaderStr << "#extension GL_EXT_shader_image_load_formatted : require\n";
    }
    if (usesBuffers) {
        // A YCbCr buffer's samples are 8- or 16-bit storage buffer elements (FLAG_INPUT_BUFFER).
        shaderStr << "#extension GL_EXT_shader_8bit_storage : require\n"
                  << "#extension GL_EXT_shader_16bit_storage : require\n";
    }
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
// The GLSL image-format qualifier to declare a binding of |format| with.
//
// vkFormatLookUp() answers nullptr for a format its table does not hold, so
// no caller below may dereference that answer for ->name. The table is not
// exhaustive over VkFormat and cannot be, so a routing change that brings a
// new single-plane format into shader generation must fail diagnosably here
// rather than on a null pointer -- the plane arms are safe only because every
// per-plane format the multi-planar table names is held, which is a property
// of two tables agreeing and not a guarantee.
//
// A format with no qualifier yields one that cannot compile, deliberately.
// CreatePipeline() already reports VK_ERROR_INITIALIZATION_FAILED when
// BuildGlslShader() rejects the generated GLSL, so an unknown format now
// fails at Init(), through the path a shader that will not compile already
// takes, and with a status the caller can act on.
//
// A storage image's declared format must be its view's (the layer reports any other as
// Undefined-Value-StorageImage-FormatMismatch-ImageView: loads and stores of the image are undefined).
// SPIR-V has no image format for B8G8R8A8_UNORM, so a view of it is declared without one: the empty
// qualifier, read with shaderStorageImageReadWithoutFormat and written with
// shaderStorageImageWriteWithoutFormat, which VulkanFilterYuvCompute::Init checks for.
static const char* GlslImageFormatQualifier(VkFormat format)
{
    if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        return "";
    }
    const VkFormatDesc* formatDesc = vkFormatLookUp(format);
    if ((formatDesc != nullptr) && (formatDesc->name != nullptr)) {
        return formatDesc->name;
    }
    assert(!"no GLSL image format qualifier for this VkFormat");
    return "NO_GLSL_IMAGE_FORMAT_QUALIFIER_FOR_THIS_VKFORMAT";
}

static void GenImageIoBindingLayout(std::stringstream& shaderStr,
                                    const char *imageName,
                                    const char *imageSubName,
                                    const char *imageFormat,
                                    bool isInput,
                                    uint32_t binding,
                                    uint32_t set,
                                    bool imageArray) {

    shaderStr << "layout (set = " << set << ", binding = " << binding;
    if (imageFormat[0] != '\0') {
        shaderStr << ", " << imageFormat;
    }
    shaderStr << ") uniform"
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
            // std430: a byte or 16-bit element array is tightly packed, as the buffer is.
            shaderStr << "layout (std430, set = " << set << ", binding = " << binding << ")"
                      << readonlyModifier << writeonlyModifier << " buffer"
                      << " " << bufferName << bufferSubName << "Buffer"
                      << " {\n"
                      << "    " << bufferDataType << " data[];\n"
                      << "} " << bufferName << bufferSubName << ";\n";
            break;

        default:
            // Unsupported buffer type
            break;
    }
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
        "    ivec2 srcPos = min(pos, ivec2(pushConstants.inputWidth - 1, pushConstants.inputHeight - 1));\n"
        "\n";
    } else {
        shaderStr <<
        "    ivec2 srcPos = pos;\n"
        "\n";
    }
}

/**
 * @brief Generates block coordinates for chroma-resolution dispatch
 *
 * Creates coordinate calculations for block-based processing where one thread
 * handles NxM luma pixels. Includes bounds checking and optional replication.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal chroma subsampling (1, 2, or 4)
 * @param chromaVertRatio Vertical chroma subsampling (1 or 2)
 * @param enableReplication Whether to enable edge pixel replication
 * @param inputChromaHorzRatio Horizontal chroma subsampling of the INPUT, or 0
 *                             for "same as the block" (same-subsampling copy)
 * @param inputChromaVertRatio Vertical chroma subsampling of the INPUT, or 0
 */
static void GenBlockCoordinates(std::stringstream& shaderStr,
                                uint32_t chromaHorzRatio,
                                uint32_t chromaVertRatio,
                                bool enableReplication,
                                uint32_t inputChromaHorzRatio,
                                uint32_t inputChromaVertRatio)
{
    // chromaHorz/VertRatio describe the OUTPUT (they set the block size and the dispatch
    // grid); inputChromaHorz/VertRatio describe the INPUT. They are equal for a
    // same-subsampling conversion and that is the only case in which the two can be used
    // interchangeably -- see srcChromaPos below.
    assert((inputChromaHorzRatio > 0) && (inputChromaVertRatio > 0));

    shaderStr <<
        "    // Block-based dispatch: one thread per chroma pixel\n"
        "    ivec2 chromaPos = ivec2(gl_GlobalInvocationID.xy);\n"
        "    \n";
    
    // Use precomputed half-resolution for 2x2 blocks (optimal case)
    if (chromaHorzRatio == 2 && chromaVertRatio == 2) {
        shaderStr <<
            "    // Use precomputed half-resolution bounds (avoid division in shader)\n"
            "    uint chromaWidth = pushConstants.halfOutputWidth;\n"
            "    uint chromaHeight = pushConstants.halfOutputHeight;\n";
    } else {
        // Fallback for non-2x2 blocks (4:2:2, etc.)
        shaderStr <<
            "    // Calculate chroma output dimensions\n"
            "    uint chromaWidth = (pushConstants.outputWidth + " << (chromaHorzRatio - 1) << ") / " << chromaHorzRatio << ";\n"
            "    uint chromaHeight = (pushConstants.outputHeight + " << (chromaVertRatio - 1) << ") / " << chromaVertRatio << ";\n";
    }
    
    shaderStr <<
        "    \n"
        "    // Bounds check at chroma resolution\n"
        "    if ((chromaPos.x >= chromaWidth) || (chromaPos.y >= chromaHeight)) {\n"
        "        return;\n"
        "    }\n"
        "    \n"
        "    // Calculate top-left luma position for this block\n"
        "    ivec2 lumaPos = chromaPos * ivec2(" << chromaHorzRatio << ", " << chromaVertRatio << ");\n"
        "    \n";

    // The position of this block's chroma sample IN THE INPUT. It is derived from lumaPos
    // and the INPUT subsampling, not from chromaPos.
    //
    // chromaPos is a coordinate on the *output* chroma grid. Indexing the input chroma
    // plane with it is correct only when both sides are subsampled identically, and it
    // fails silently when they are not: for NV12 -> NV16 it walks the output's 0..h-1
    // chroma rows against an input plane only h/2 tall, so the top half of the frame is
    // exact and everything below reads past the end of the plane, while NV12 -> YUV444
    // over-runs both axes. inputChromaMax must come from the input ratio for the same
    // reason.
    shaderStr <<
        "    // Input chroma position: from lumaPos and the INPUT subsampling ("
        << inputChromaHorzRatio << "x" << inputChromaVertRatio << "),\n"
        "    // which is not the output/block ratio unless the two formats agree.\n"
        "    ivec2 srcChromaPos = lumaPos / ivec2("
        << inputChromaHorzRatio << ", " << inputChromaVertRatio << ");\n";

    // Generate input bounds for replication
    if (enableReplication) {
        shaderStr <<
            "    // Input bounds for edge replication\n"
            "    ivec2 inputLumaMax = ivec2(pushConstants.inputWidth - 1, pushConstants.inputHeight - 1);\n";

        // Use precomputed half-resolution for input chroma bounds (4:2:0 input)
        if ((inputChromaHorzRatio == 2) && (inputChromaVertRatio == 2)) {
            shaderStr <<
                "    ivec2 inputChromaMax = ivec2(pushConstants.halfInputWidth - 1, pushConstants.halfInputHeight - 1);\n";
        } else {
            shaderStr <<
                "    ivec2 inputChromaMax = ivec2((pushConstants.inputWidth + " << (inputChromaHorzRatio-1) << ") / " << inputChromaHorzRatio << " - 1, "
                "(pushConstants.inputHeight + " << (inputChromaVertRatio-1) << ") / " << inputChromaVertRatio << " - 1);\n";
        }

        shaderStr << "    srcChromaPos = min(srcChromaPos, inputChromaMax);\n";
    }

    shaderStr << "    \n";
}

/**
 * @brief Generates code to read a block of Y pixels and chroma
 *
 * Reads NxM Y pixels and 1 chroma sample for block-based processing.
 * Handles replication, 2-plane vs 3-plane, and missing chroma.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 * @param isInputTwoPlane Whether input is 2-plane (NV12) or 3-plane (I420)
 * @param hasInputChroma Whether input has chroma planes
 * @param enableReplication Whether to clamp reads for edge replication
 */
static void GenReadYCbCrBlock(std::stringstream& shaderStr,
                              uint32_t chromaHorzRatio,
                              uint32_t chromaVertRatio,
                              bool isInputTwoPlane,
                              bool hasInputChroma,
                              bool enableReplication,
                              uint32_t inputChromaHorzSubsampling = 2,
                              uint32_t inputChromaVertSubsampling = 2,
                              const VkPackedYcbcrFormatInfo* packedInput = nullptr,
                              bool inputIsBuffer = false)
{
    // Packed (single-plane) input: one imageLoad per pixel from the single COLOR_BIT
    // image declared as <name>RGB, with Y/Cb/Cr taken from the channels the format
    // actually uses. The planar arms below reference inputImageY / inputImageCbCr, which
    // are NOT declared for a packed side -- that mismatch is the 'inputImageY : undeclared
    // identifier' failure this branch exists to prevent.
    //
    // Chroma is box-averaged over the block, matching what the planar arms do when the
    // input is more subsampled than the block. Values stay raw UNORM in [0,1] centred at
    // 0.5, exactly like the planar arms -- do NOT apply the -0.5 bias used by
    // InitRGBA2YCBCR, whose intermediate is [-0.5,0.5].
    if (packedInput != nullptr) {
        const uint32_t blockPixels = chromaHorzRatio * chromaVertRatio;
        shaderStr << "    // Read " << chromaHorzRatio << "x" << chromaVertRatio
                  << " packed " << packedInput->debugName << " block\n"
                  << "    float cb = 0.0;\n"
                  << "    float cr = 0.0;\n";
        for (uint32_t y = 0; y < chromaVertRatio; y++) {
            for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                // ivec2, not ivec3: inputImageRGB is the single COLOR_BIT
                // binding, which ShaderGenerateImagePlaneDescriptors declares
                // image2D because it is bound from the combined
                // VK_IMAGE_VIEW_TYPE_2D view. pushConstants.srcLayer has nothing
                // to select -- a 2D view carries the one layer, which is the
                // layer the ivec3 form was reading anyway.
                shaderStr << "    vec4 pk" << x << y << " = imageLoad(inputImageRGB, ";
                if (enableReplication) {
                    shaderStr << "min(lumaPos + ivec2(" << x << ", " << y << "), inputLumaMax)";
                } else {
                    shaderStr << "lumaPos + ivec2(" << x << ", " << y << ")";
                }
                shaderStr << ");\n"
                          << "    float y" << x << y << " = pk" << x << y << "."
                          << PackedChan(packedInput->yChannel) << ";\n"
                          << "    cb += pk" << x << y << "." << PackedChan(packedInput->cbChannel) << ";\n"
                          << "    cr += pk" << x << y << "." << PackedChan(packedInput->crChannel) << ";\n";
            }
        }
        shaderStr << "    cb *= " << (1.0f / (float)blockPixels) << ";\n"
                  << "    cr *= " << (1.0f / (float)blockPixels) << ";\n"
                  << "    \n";
        return;
    }

    // A sample of a plane at a position: a buffer input reads it through the accessors
    // GenYCbCrBufferAccessors() defines, an image input loads it from its plane view.
    auto loadPlane = [inputIsBuffer](const char* plane, const char* swizzle, const std::string& pos) {
        return inputIsBuffer ? (std::string("readIn") + plane + "(" + pos + ")")
                             : (std::string("imageLoad(inputImage") + plane + ", ivec3(" + pos +
                                ", pushConstants.srcLayer))." + swizzle);
    };
    auto blockPos = [enableReplication](uint32_t x, uint32_t y) {
        const std::string pos = "lumaPos + ivec2(" + std::to_string(x) + ", " + std::to_string(y) + ")";
        return enableReplication ? ("min(" + pos + ", inputLumaMax)") : pos;
    };

    shaderStr << "    // Read " << chromaHorzRatio << "x" << chromaVertRatio << " Y block\n";

    // Read all Y pixels in the block
    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            shaderStr << "    float y" << x << y << " = " << loadPlane("Y", "r", blockPos(x, y)) << ";\n";
        }
    }

    shaderStr << "    \n";

    // Read chroma for the block
    if (hasInputChroma) {
        bool isInput444 = (inputChromaHorzSubsampling == 1 && inputChromaVertSubsampling == 1);
        
        if (isInput444) {
            // For 4:4:4 input, read chroma at each Y pixel position
            shaderStr << "    // Read " << chromaHorzRatio << "x" << chromaVertRatio << " chroma samples (4:4:4 input)\n";
            if (isInputTwoPlane) {
                // Read CbCr from all positions and average
                shaderStr << "    float cb = 0.0;\n"
                          << "    float cr = 0.0;\n";
                for (uint32_t y = 0; y < chromaVertRatio; y++) {
                    for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                        shaderStr << "    vec2 cbcr" << x << y << " = " << loadPlane("CbCr", "rg", blockPos(x, y)) << ";\n";
                        shaderStr << "    cb += cbcr" << x << y << ".r;\n";
                        shaderStr << "    cr += cbcr" << x << y << ".g;\n";
                    }
                }
                // A 1x1 block is one sample, not an average.
                if ((chromaHorzRatio * chromaVertRatio) > 1) {
                    float scale = 1.0f / (chromaHorzRatio * chromaVertRatio);
                    shaderStr << "    cb *= " << scale << ";\n";
                    shaderStr << "    cr *= " << scale << ";\n";
                }
            } else {
                // 3-plane format: average all Cb and Cr samples
                shaderStr << "    float cb = 0.0;\n"
                          << "    float cr = 0.0;\n";
                for (uint32_t y = 0; y < chromaVertRatio; y++) {
                    for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                        shaderStr << "    float cb" << x << y << " = " << loadPlane("Cb", "r", blockPos(x, y)) << ";\n";
                        shaderStr << "    float cr" << x << y << " = " << loadPlane("Cr", "r", blockPos(x, y)) << ";\n";
                        shaderStr << "    cb += cb" << x << y << ";\n";
                        shaderStr << "    cr += cr" << x << y << ";\n";
                    }
                }
                // A 1x1 block is one sample, not an average -- emitting
                // "cb *= 1" there would be noise in the generated shader.
                if ((chromaHorzRatio * chromaVertRatio) > 1) {
                    float scale = 1.0f / (chromaHorzRatio * chromaVertRatio);
                    shaderStr << "    cb *= " << scale << ";\n";
                    shaderStr << "    cr *= " << scale << ";\n";
                }
            }
        } else {
            // Subsampled input (4:2:0, 4:2:2). The block covers this many input chroma
            // samples in each axis; more than one means the output is MORE subsampled
            // than the input, so they have to be box-averaged rather than point-sampled
            // (e.g. 4:2:2 -> 4:2:0 spans two input chroma rows). At least one, because
            // an output that is LESS subsampled replicates a single sample instead.
            const uint32_t srcSamplesX = std::max(1u, chromaHorzRatio / inputChromaHorzSubsampling);
            const uint32_t srcSamplesY = std::max(1u, chromaVertRatio / inputChromaVertSubsampling);
            const uint32_t srcSamples  = srcSamplesX * srcSamplesY;

            if (srcSamples == 1) {
                if (isInputTwoPlane) {
                    shaderStr << "    // Read 1 CbCr sample (2-plane format)\n"
                              << "    vec2 cbcr = " << loadPlane("CbCr", "rg", "srcChromaPos") << ";\n"
                              << "    float cb = cbcr.r;\n"
                              << "    float cr = cbcr.g;\n";
                } else {
                    shaderStr << "    // Read 1 Cb and 1 Cr sample (3-plane format)\n"
                              << "    float cb = " << loadPlane("Cb", "r", "srcChromaPos") << ";\n"
                              << "    float cr = " << loadPlane("Cr", "r", "srcChromaPos") << ";\n";
                }
            } else {
                shaderStr << "    // Box-average " << srcSamplesX << "x" << srcSamplesY
                          << " input chroma samples (output is more subsampled than input)\n"
                          << "    float cb = 0.0;\n"
                          << "    float cr = 0.0;\n";
                for (uint32_t sy = 0; sy < srcSamplesY; sy++) {
                    for (uint32_t sx = 0; sx < srcSamplesX; sx++) {
                        shaderStr << "    {\n        ivec2 sp = srcChromaPos + ivec2(" << sx << ", " << sy << ");\n";
                        if (enableReplication) {
                            shaderStr << "        sp = min(sp, inputChromaMax);\n";
                        }
                        if (isInputTwoPlane) {
                            shaderStr << "        vec2 s = " << loadPlane("CbCr", "rg", "sp") << ";\n"
                                      << "        cb += s.r;\n        cr += s.g;\n";
                        } else {
                            shaderStr << "        cb += " << loadPlane("Cb", "r", "sp") << ";\n"
                                      << "        cr += " << loadPlane("Cr", "r", "sp") << ";\n";
                        }
                        shaderStr << "    }\n";
                    }
                }
                shaderStr << "    cb *= " << (1.0f / (float)srcSamples) << ";\n"
                          << "    cr *= " << (1.0f / (float)srcSamples) << ";\n";
            }
        }
    } else {
        shaderStr << "    // No chroma in input, use default (128/255 limited range)\n"
                  << "    float cb = 128.0/255.0;\n"
                  << "    float cr = 128.0/255.0;\n";
    }

    shaderStr << "    \n";
}

/**
 * @brief Generates code to convert a block of YCbCr pixels
 *
 * Applies format conversion (bit depth / range) to all Y pixels and chroma.
 * Uses convertYCbCrFormat() helper if conversion is needed.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 * @param needsConversion Whether format conversion is needed
 */
static void GenConvertYCbCrBlock(std::stringstream& shaderStr,
                                 uint32_t chromaHorzRatio,
                                 uint32_t chromaVertRatio,
                                 bool needsConversion,
                                 bool hasChroma = true)
{
    if (needsConversion) {
        shaderStr << "    // Apply format conversion to block\n";
        // Convert each Y pixel
        for (uint32_t y = 0; y < chromaVertRatio; y++) {
            for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                shaderStr << "    float yOut" << x << y << " = convertYCbCrFormat(vec3(y" << x << y << ", cb, cr)).x;\n";
            }
        }
        if (hasChroma) {
            // Convert chroma once
            shaderStr << "    vec2 cbcrOut = convertYCbCrFormat(vec3(y00, cb, cr)).yz;\n";
        }
    } else {
        shaderStr << "    // No conversion needed - direct copy\n";
        for (uint32_t y = 0; y < chromaVertRatio; y++) {
            for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                shaderStr << "    float yOut" << x << y << " = y" << x << y << ";\n";
            }
        }
        if (hasChroma) {
            shaderStr << "    vec2 cbcrOut = vec2(cb, cr);\n";
        }
    }

    shaderStr << "    \n";
}

/**
 * @brief Generates code to apply MSB-to-LSB bit shift for block INPUT
 *
 * The mirror of GenApplyBlockOutputShift, and the reason that one is safe.
 *
 * The block path reads its samples with imageLoad through per-plane
 * VK_FORMAT_R16_UNORM views. An X6/X4-packed source stores a 10/12-bit sample
 * MSB-aligned in a 16-bit word, so that load already returns a correctly
 * normalized value -- (pixel << 6) / 65535 ~= pixel / 1023 at 10-bit. The
 * output stage then multiplies by the same factor to re-align for the write.
 * With only the output half present the sample is scaled by 64 (or 16) with
 * nothing undoing it, and every channel saturates.

 *
 * Applied immediately after the read and before any conversion, so the
 * conversion stage sees LSB-aligned values, which is what its normalization
 * helpers assume.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 * @param inputBitDepth Input bit depth (8, 10, 12, or 16)
 * @param enableShift Whether MSB-to-LSB shift is enabled
 * @param hasChroma Whether the input carries chroma
 */
static void GenApplyBlockInputShift(std::stringstream& shaderStr,
                                    uint32_t chromaHorzRatio,
                                    uint32_t chromaVertRatio,
                                    uint32_t inputBitDepth,
                                    bool enableShift,
                                    bool hasChroma = true)
{
    if (!enableShift) {
        return;
    }

    // Exact reciprocals of the output stage's factors: both are powers of two,
    // so the round trip is an identity in float rather than an approximation.
    float shiftRecip = 0.0f;
    if (inputBitDepth == 10) {
        shiftRecip = 1.0f / 64.0f;   // 1 >> (16 - 10)
    } else if (inputBitDepth == 12) {
        shiftRecip = 1.0f / 16.0f;   // 1 >> (16 - 12)
    } else {
        return;  // No shift for 8 or 16-bit
    }

    shaderStr << "    // Apply MSB-to-LSB shift for " << inputBitDepth << "-bit input\n";

    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            shaderStr << "    y" << x << y << " *= " << shiftRecip << ";\n";
        }
    }
    if (hasChroma) {
        // cb/cr are already the averaged samples where the input needed
        // averaging; scaling is linear, so applying it here is equivalent to
        // scaling each contributor before the sum.
        shaderStr << "    cb *= " << shiftRecip << ";\n";
        shaderStr << "    cr *= " << shiftRecip << ";\n";
    }

    shaderStr << "    \n";
}

/**
 * @brief Generates code to apply LSB-to-MSB bit shift for block output
 *
 * For 10/12-bit output, shifts values from LSB-aligned to MSB-aligned format.
 * Multiplies all Y pixels and chroma by shift factor.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 * @param outputBitDepth Output bit depth (8, 10, 12, or 16)
 * @param enableShift Whether LSB-to-MSB shift is enabled
 */
static void GenApplyBlockOutputShift(std::stringstream& shaderStr,
                                     uint32_t chromaHorzRatio,
                                     uint32_t chromaVertRatio,
                                     uint32_t outputBitDepth,
                                     bool enableShift,
                                     bool hasChroma = true)
{
    if (!enableShift) {
        return;
    }

    float shiftFactor = 0.0f;
    if (outputBitDepth == 10) {
        shiftFactor = 64.0f;  // 1 << (16 - 10)
    } else if (outputBitDepth == 12) {
        shiftFactor = 16.0f;  // 1 << (16 - 12)
    } else {
        return;  // No shift for 8 or 16-bit
    }

    shaderStr << "    // Apply LSB-to-MSB shift for " << outputBitDepth << "-bit output\n";

    // Shift all Y pixels
    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            shaderStr << "    yOut" << x << y << " *= " << shiftFactor << ";\n";
        }
    }
    if (hasChroma) {
        // Shift chroma
        shaderStr << "    cbcrOut *= " << shiftFactor << ";\n";
    }
    shaderStr << "    \n";
}

/**
 * @brief Generates code to write a block of Y pixels and chroma
 *
 * Writes NxM Y pixels and 1 chroma sample to output.
 * Handles 2-plane vs 3-plane output formats.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 * @param isOutputTwoPlane Whether output is 2-plane or 3-plane
 * @param hasOutputChroma Whether output has chroma planes
 */
// The index an OUTPUT imageStore takes: ivec2 for an image2D binding, and
// ivec3(..., dstLayer) for an image2DArray one. Emitting the wrong one is
// VUID-vkCmdDispatch-viewType-07752 in one direction, or a shader that does not
// compile in the other, so every output store derives its form from the same
// predicate -- VulkanFilterYuvCompute::OutputDescriptorIsArray().
static std::string OutStoreIndex(const std::string& xy, bool isArray)
{
    return isArray ? ("ivec3(" + xy + ", pushConstants.dstLayer)") : xy;
}

static void GenWriteYCbCrBlock(std::stringstream& shaderStr,
                               uint32_t ySubsampleHorzRatio,
                               uint32_t ySubsampleVertRatio,
                               bool isOutputTwoPlane,
                               bool hasOutputChroma,
                               uint32_t outputChromaHorzSubsampling = 2,
                               uint32_t outputChromaVertSubsampling = 2,
                               const VkPackedYcbcrFormatInfo* packedOutput = nullptr,
                               bool outputIsArray = false,
                               bool outputIsBuffer = false)
{
    // Packed (single-plane) output: one imageStore per luma pixel into the single
    // COLOR_BIT image declared as <name>RGB, with Y/Cb/Cr placed in the channels the
    // format uses. There is no separate chroma store -- 4:4:4 packed carries a full
    // chroma sample per pixel. Mirrors the packed read arm in GenReadYCbCrBlock; without
    // it the same 'undeclared identifier' fires on outputImageY.
    if (packedOutput != nullptr) {
        shaderStr << "    // Write " << ySubsampleHorzRatio << "x" << ySubsampleVertRatio
                  << " packed " << packedOutput->debugName << " pixels\n";
        for (uint32_t y = 0; y < ySubsampleVertRatio; y++) {
            for (uint32_t x = 0; x < ySubsampleHorzRatio; x++) {
                // Build the vec4 by channel index so the order comes from the table, not
                // from a per-format hand-written store.
                const char* comp[4] = { "0.0", "0.0", "0.0", "1.0" };
                std::string yExpr  = std::string("yOut") + std::to_string(x) + std::to_string(y);
                comp[packedOutput->yChannel & 3]  = yExpr.c_str();
                comp[packedOutput->cbChannel & 3] = hasOutputChroma ? "cbcrOut.x" : "0.5";
                comp[packedOutput->crChannel & 3] = hasOutputChroma ? "cbcrOut.y" : "0.5";
                // ivec2, not ivec3 -- the mirror of the packed read arm's
                // index form, and for the same reason: outputImageRGB is the
                // combined VK_IMAGE_VIEW_TYPE_2D view's binding, declared
                // image2D.
                shaderStr << "    imageStore(outputImageRGB, "
                          << OutStoreIndex("lumaPos + ivec2(" + std::to_string(x) + ", " +
                                           std::to_string(y) + ")", outputIsArray)
                          << ", vec4("
                          << comp[0] << ", " << comp[1] << ", " << comp[2] << ", " << comp[3] << "));\n";
            }
        }
        shaderStr << "    \n";
        return;
    }

    shaderStr << "    // Write " << ySubsampleHorzRatio << "x" << ySubsampleVertRatio << " Y pixels\n";

    // Write all Y pixels. A buffer output writes through the bounded accessors
    // GenYCbCrBufferAccessors() defines, where an image store outside the image is dropped.
    for (uint32_t y = 0; y < ySubsampleVertRatio; y++) {
        for (uint32_t x = 0; x < ySubsampleHorzRatio; x++) {
            if (outputIsBuffer) {
                shaderStr << "    writeOutY(lumaPos + ivec2(" << x << ", " << y << "), yOut" << x << y << ");\n";
                continue;
            }
            shaderStr <<
                "    imageStore(outputImageY, ivec3(lumaPos + ivec2(" << x << ", " << y << "), pushConstants.dstLayer), "
                "vec4(yOut" << x << y << ", 0, 0, 1));\n";
        }
    }

    shaderStr << "    \n";

    // Write chroma
    if (!hasOutputChroma) {
        return;
    }

    // For 4:4:4 output (no chroma subsampling), write chroma at each luma position
    bool isOutput444 = (outputChromaHorzSubsampling == 1 && outputChromaVertSubsampling == 1);
    
    if (isOutput444) {
        // Write chroma at each Y pixel position
        shaderStr << "    // Write " << ySubsampleHorzRatio << "x" << ySubsampleVertRatio << " chroma pixels (4:4:4 format)\n";
        for (uint32_t y = 0; y < ySubsampleVertRatio; y++) {
            for (uint32_t x = 0; x < ySubsampleHorzRatio; x++) {
                if (outputIsBuffer) {
                    const std::string pos = "lumaPos + ivec2(" + std::to_string(x) + ", " + std::to_string(y) + ")";
                    if (isOutputTwoPlane) {
                        shaderStr << "    writeOutCbCr(" << pos << ", cbcrOut);\n";
                    } else {
                        shaderStr << "    writeOutCb(" << pos << ", cbcrOut.x);\n"
                                  << "    writeOutCr(" << pos << ", cbcrOut.y);\n";
                    }
                } else if (isOutputTwoPlane) {
                    shaderStr <<
                        "    imageStore(outputImageCbCr, ivec3(lumaPos + ivec2(" << x << ", " << y << "), pushConstants.dstLayer), "
                        "vec4(cbcrOut.x, cbcrOut.y, 0, 1));\n";
                } else {
                    shaderStr <<
                        "    imageStore(outputImageCb, ivec3(lumaPos + ivec2(" << x << ", " << y << "), pushConstants.dstLayer), "
                        "vec4(cbcrOut.x, 0, 0, 1));\n"
                        "    imageStore(outputImageCr, ivec3(lumaPos + ivec2(" << x << ", " << y << "), pushConstants.dstLayer), "
                        "vec4(cbcrOut.y, 0, 0, 1));\n";
                }
            }
        }
    } else {
        // Write 1 chroma sample for subsampled formats (4:2:0, 4:2:2)
        if (outputIsBuffer) {
            if (isOutputTwoPlane) {
                shaderStr << "    // Write 1 CbCr sample (2-plane format)\n"
                          << "    writeOutCbCr(chromaPos, cbcrOut);\n";
            } else {
                shaderStr << "    // Write 1 Cb and 1 Cr sample (3-plane format)\n"
                          << "    writeOutCb(chromaPos, cbcrOut.x);\n"
                          << "    writeOutCr(chromaPos, cbcrOut.y);\n";
            }
        } else if (isOutputTwoPlane) {
            shaderStr <<
                "    // Write 1 CbCr sample (2-plane format)\n"
                "    imageStore(outputImageCbCr, ivec3(chromaPos, pushConstants.dstLayer), "
                "vec4(cbcrOut.x, cbcrOut.y, 0, 1));\n";
        } else {
            shaderStr <<
                "    // Write 1 Cb and 1 Cr sample (3-plane format)\n"
                "    imageStore(outputImageCb, ivec3(chromaPos, pushConstants.dstLayer), vec4(cbcrOut.x, 0, 0, 1));\n"
                "    imageStore(outputImageCr, ivec3(chromaPos, pushConstants.dstLayer), vec4(cbcrOut.y, 0, 0, 1));\n";
        }
    }

    shaderStr << "    \n";
}

/**
 * @brief Generates code to compute and write subsampled Y
 *
 * Computes box filter average of NxM Y pixels and writes to binding 9.
 *
 * @param shaderStr Output stringstream
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 */
static void GenComputeAndWriteSubsampledY(std::stringstream& shaderStr,
                                          uint32_t chromaHorzRatio,
                                          uint32_t chromaVertRatio)
{
    float scale = 1.0f / (chromaHorzRatio * chromaVertRatio);

    shaderStr << "    // Compute " << chromaHorzRatio << "x" << chromaVertRatio << " box filter for subsampled Y\n"
              << "    float subsampledY = (";

    bool first = true;
    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            if (!first) shaderStr << " + ";
            shaderStr << "yOut" << x << y;
            first = false;
        }
    }

    shaderStr <<
        ") * " << scale << ";\n"
        "    imageStore(subsampledImageY, chromaPos, vec4(subsampledY, 0, 0, 1));\n"
        "    \n";
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

    // The caller must pass imageArray matching the VIEW its descriptor will bind:
    //
    //   input/output PLANE bindings  -> VkImageResourceView::GetPlaneImageView()
    //        -> per-plane storage views, always VK_IMAGE_VIEW_TYPE_2D_ARRAY
    //           (VkImageResource.cpp, both Create() overloads) -> imageArray = true
    //   the subsampled-Y binding      -> VkImageResourceView::GetImageView()
    //        -> the combined view, which follows layerCount   -> imageArray = false
    //   color/RGB bindings            -> GetImageView() as well -> follows layerCount
    //
    // A view's type must match the Dim/Arrayed operands of the shader's OpTypeImage
    // (VUID-vkCmdDispatch-viewType-07752). Note the subsampled binding is declared
    // through this same PLANE_0 path but is bound from the combined view, so "plane
    // aspect" alone does not imply an array view.

    shaderStr << " // The " << (isInput ? "input" : "output") << " image binding\n";

    // Handle the Y only and CbCr only images
    switch(imageFormat) {
    case VK_FORMAT_R8_UNORM:
        // fallthrough
    case VK_FORMAT_R16_UNORM:
        imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT;

        GenImageIoBindingLayout(shaderStr, imageName, "Y",
                                       GlslImageFormatQualifier(imageFormat),
                                       isInput,
                                       ++startBinding,
                                       set,
                                       imageArray);
        return startBinding;
    case VK_FORMAT_R8G8_UNORM:
        // fallthrough
    case VK_FORMAT_R16G16_UNORM:

        imageAspects = VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;

        GenImageIoBindingLayout(shaderStr, imageName, "CbCr",
                                GlslImageFormatQualifier(imageFormat),
                                isInput,
                                ++startBinding,
                                set,
                                imageArray);


        // fallthrough
        return startBinding;
    default:
        break;
    }

    // Image binding goes in this pattern:
    // offset 0: RGBA image
    // offset 1: multi-planar image plane Y
    // offset 2: 2-planar image plane CbCr or 3-planar image plane Cb
    // offset 3: 3-planar image plane Cr
    const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(imageFormat);
    if (inputMpInfo) {

        GenImageIoBindingLayout(shaderStr, imageName, "Y",
                                GlslImageFormatQualifier(inputMpInfo->vkPlaneFormat[0]),
                                isInput,
                                ++startBinding,
                                set,
                                imageArray);

        if (inputMpInfo->planesLayout.numberOfExtraPlanes == 1) {

            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

            GenImageIoBindingLayout(shaderStr, imageName, "CbCr",
                                    GlslImageFormatQualifier(inputMpInfo->vkPlaneFormat[1]),
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);

        } else if (inputMpInfo->planesLayout.numberOfExtraPlanes == 2) {

            imageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                         VK_IMAGE_ASPECT_PLANE_2_BIT;

            GenImageIoBindingLayout(shaderStr, imageName, "Cb",
                                    GlslImageFormatQualifier(inputMpInfo->vkPlaneFormat[1]),
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);

            GenImageIoBindingLayout(shaderStr, imageName, "Cr",
                                    GlslImageFormatQualifier(inputMpInfo->vkPlaneFormat[2]),
                                    isInput,
                                    ++startBinding,
                                    set,
                                    imageArray);
        }
    } else {

        imageAspects = VK_IMAGE_ASPECT_COLOR_BIT;

        // NOT |imageArray|, and this is the one arm where a per-call answer
        // cannot be right. A COLOR_BIT binding is the SINGLE-PLANE arm, and
        // UpdateImageDescriptorSets binds every COLOR_BIT descriptor from
        // GetImageView() -- the COMBINED view, which follows the image's own
        // layerCount. Declaring image2DArray over a VK_IMAGE_VIEW_TYPE_2D view
        // is VUID-vkCmdDispatch-viewType-07752 and the access is UNDEFINED;
        // this driver returns layer 0, which is why the picture was right and
        // the fault survived. The plane arms above keep the caller's request,
        // and must: they bind GetPlaneImageView(), which Create() declares
        // VK_IMAGE_VIEW_TYPE_2D_ARRAY unconditionally.
        //
        // Deciding it HERE rather than at each call site is the point. This
        // function chooses the arm, so only it knows which one was taken; a
        // caller can only re-derive that from the format, and two places
        // answering the same question independently is what went wrong --
        // InitYCBCRCOPY asked for an array on both of its sides, which is right
        // for a planar side and wrong for a packed one.
        //
        // OutputDescriptorIsArray() IS THAT ONE PLACE, and the OUTPUT side must
        // use it rather than a constant: every output imageStore already
        // derives its index form from it through OutStoreIndex(). Hardcoding
        // false here meant that a caller passing FLAG_OUTPUT_IMAGE_ARRAY got
        // ivec3 stores against an image2D declaration -- GLSL that does not
        // compile. With the flag unset the predicate is false and this arm
        // declares image2D exactly as before, which is every caller in tree.
        //
        // The INPUT side keeps ivec2: there is no input-side flag, so its
        // combined view is single-layer by construction.
        GenImageIoBindingLayout(shaderStr, imageName, "RGB",
                                GlslImageFormatQualifier(imageFormat),
                                isInput,
                                startBinding++,
                                set,
                                isInput ? false : OutputDescriptorIsArray());
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

    // The elements GenBufferAccessFuncs() reads and writes: a byte per 8-bit YCbCr sample, a 16-bit word
    // per deeper one, a 32-bit word per RGBA pixel (two for R16G16B16A16).
    const char* elementType = (inputMpInfo == nullptr) ? "uint" :
                              ((GetBitsPerChannel(inputMpInfo->planesLayout) > 8) ? "uint16_t" : "uint8_t");

    shaderStr << " // The " << (isInput ? "input" : "output") << " buffer binding\n";

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
/**
 * @brief The ITU-R code levels a normalized [0,1] Y'CbCr value maps onto
 *
 * One definition shared by everything that has to agree on what "narrow range" means.
 * The limited-range levels are the 8-bit ITU-R definition (Y[16,235], Cb/Cr[16,240])
 * scaled by 2^(bitDepth-8), which reproduces the published constants exactly:
 * 10-bit Y[64,940] C[64,960], 12-bit Y[256,3760] C[256,3840], 16-bit Y[4096,60160].
 */
struct YCbCrRangeLevels {
    double maxValue;   // full-scale code value for the bit depth
    double yBlack;     // luma code for black
    double yWhite;     // luma code for white
    double cZero;      // chroma code at the bottom of the chroma excursion
    double cScale;     // chroma excursion, i.e. (cMax - cZero)
};

static YCbCrRangeLevels GetYCbCrRangeLevels(uint32_t bitDepth, bool isLimitedRange)
{
    YCbCrRangeLevels levels = {};
    levels.maxValue = (double)((1ULL << bitDepth) - 1ULL);

    if (!isLimitedRange) {
        levels.yBlack = 0.0;
        levels.yWhite = levels.maxValue;
        levels.cZero  = 0.0;
        levels.cScale = levels.maxValue;
        return levels;
    }

    // Only 8/10/12/16 are real Y'CbCr depths; anything else falls back to the 8-bit
    // levels rather than shifting by a negative amount.
    const bool depthIsSupported = (bitDepth == 8) || (bitDepth == 10) ||
                                  (bitDepth == 12) || (bitDepth == 16);
    assert(depthIsSupported);
    const double scale = depthIsSupported ? (double)(1ULL << (bitDepth - 8)) : 1.0;

    levels.yBlack =  16.0 * scale;
    levels.yWhite = 235.0 * scale;
    levels.cZero  =  16.0 * scale;
    levels.cScale = 224.0 * scale;
    return levels;
}

static void GenYCbCrNormalizationFuncs(std::stringstream& shaderStr,
                                       uint32_t bitDepth = 8,
                                       bool isLimitedRange = true,
                                       bool hasChroma = true)
{
    // STEP 1: Calculate normalization parameters based on bit depth and range
    // ===========================================================================

    // Use double precision for calculations to maintain precision.
    // Values come from the shared table so the generators and the RGBA->YCbCr
    // range mapping cannot disagree.
    const YCbCrRangeLevels rangeC = GetYCbCrRangeLevels(bitDepth, isLimitedRange);
    const double maxValue = rangeC.maxValue;
    const double yBlack   = rangeC.yBlack;
    const double yWhite   = rangeC.yWhite;
    const double cZero    = rangeC.cZero;
    const double cScale   = rangeC.cScale;

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
 * @brief Generates convertYCbCrFormat(), in normalized space
 *
 * imageLoad() on a UNORM image and a buffer's loadIn*() (GenBufferAccessFuncs) return a
 * normalized [0,1] sample, and imageStore() and storeOut*() expect one, so the loads and
 * stores do all the bit-depth scaling. The only thing that can need converting between
 * the two sides is the RANGE, and that is one affine map.
 *
 * Per component, converting between stored representations:
 *     stored = true * scale + offset   =>   out = in * (outScale/inScale)
 *                                                 + (outOffset - inOffset*outScale/inScale)
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 */
static void GenConvertYCbCrRangeNormalized(std::stringstream& shaderStr,
                                           uint32_t inputBitDepth, bool isInputLimitedRange,
                                           uint32_t outputBitDepth, bool isOutputLimitedRange,
                                           bool hasChroma)
{
    const YCbCrRangeLevels in  = GetYCbCrRangeLevels(inputBitDepth,  isInputLimitedRange);
    const YCbCrRangeLevels out = GetYCbCrRangeLevels(outputBitDepth, isOutputLimitedRange);

    const double inScaleY   = (in.yWhite  - in.yBlack)  / in.maxValue;
    const double inOffsetY  =  in.yBlack  / in.maxValue;
    const double outScaleY  = (out.yWhite - out.yBlack) / out.maxValue;
    const double outOffsetY =  out.yBlack / out.maxValue;

    // Chroma is stored centred on its own excursion, not on zero.
    const double inScaleC   = in.cScale  / in.maxValue;
    const double inCenterC  = (in.cZero  + (in.cScale  / 2.0)) / in.maxValue;
    const double outScaleC  = out.cScale / out.maxValue;
    const double outCenterC = (out.cZero + (out.cScale / 2.0)) / out.maxValue;

    const double scaleY  = outScaleY / inScaleY;
    const double offsetY = outOffsetY - (inOffsetY * scaleY);
    const double scaleC  = outScaleC / inScaleC;
    const double offsetC = outCenterC - (inCenterC * scaleC);

    std::stringstream ss;
    ss.precision(16);
    ss << std::fixed;

    ss << "// Y'CbCr range conversion, image -> image (normalized domain; the UNORM store\n"
       << "// handles bit depth, so " << inputBitDepth << "-bit -> " << outputBitDepth
       << "-bit needs no scaling of its own)\n"
       << "vec3 convertYCbCrFormat(vec3 ycbcr) {\n"
       << "    return ycbcr * vec3(" << scaleY << ", " << scaleC << ", " << scaleC << ")\n"
       << "                 + vec3(" << offsetY << ", " << offsetC << ", " << offsetC << ");\n"
       << "}\n\n";

    (void)hasChroma;   // chroma rides the same vec3; nothing extra to emit
    shaderStr << ss.str();
}

VkFormat VulkanFilterYuvCompute::GetOutputFormat(FilterType filterType, VkFormat inputFormat, VkFormat outputFormat)
{
    if (filterType == YCBCR2RGBA) {
        // The format the caller writes the output in, which the shader declares its output with;
        // derived from the input's depth only when the caller names no RGBA format. A single-plane
        // format is RGBA here: this filter's output is never packed YCbCr, whose carrier formats
        // (R8G8B8A8_UNORM for AYUV) are RGBA ones too.
        if ((outputFormat != VK_FORMAT_UNDEFINED) && (YcbcrVkFormatInfo(outputFormat) == nullptr)) {
            return outputFormat;
        }
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(inputFormat);
        const VkPackedYcbcrFormatInfo* packedInfo = (mpInfo == nullptr) ? PackedYcbcrFormatInfo(inputFormat) : nullptr;
        if (packedInfo != nullptr)
            return (packedInfo->bitDepth > 8) ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        if (mpInfo == nullptr)
            return VK_FORMAT_UNDEFINED;
        if (mpInfo->planesLayout.bpp == YCBCRA_8BPP)
            return VK_FORMAT_R8G8B8A8_UNORM;
        // 10/12/16-bit YCbCr -> RGBA16
        return VK_FORMAT_R16G16B16A16_UNORM;
    }
    return outputFormat;
}

bool VulkanFilterYuvCompute::OutputDescriptorIsArray() const
{
    // A MULTI-PLANAR output binds per-plane views. VkImageResourceView::Create
    // types those VK_IMAGE_VIEW_TYPE_2D_ARRAY unconditionally, in both of its
    // overloads, so the declaration is an array image whatever the layer count.
    //
    // A SINGLE-PLANE output -- packed YCbCr (AYUV, Y410) or RGBA -- has no
    // per-plane view to bind, so it binds the COMBINED view, and that one
    // follows the image's layerCount. The caller declares it with
    // FLAG_OUTPUT_IMAGE_ARRAY, because the shader is generated before any image
    // is bound and the format alone cannot answer it.
    const VkMpFormatInfo* mpOutputInfo = YcbcrVkFormatInfo(m_outputFormat);
    const bool bindsPerPlaneViews =
        (mpOutputInfo != nullptr) && (m_outputPackedYcbcr == nullptr);
    return bindsPerPlaneViews ? true : (m_outputImageArray != 0);
}

// 32-bit words per pixel of an RGBA buffer (FLAG_INPUT_BUFFER); 0 for a format the buffer access has not.
static uint32_t RgbaBufferWordsPerPixel(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
        return 1;
    case VK_FORMAT_R16G16B16A16_UNORM:
        return 2;
    default:
        return 0;
    }
}

// GLSL that loads (input) or stores (output) the samples of a buffer, one function per plane taking the index of
// an element: loadInY(), loadInCbCr(), or loadInCb() and loadInCr(), loadInRGBA(), and storeOut...() alike. A
// YCbCr sample is normalized as an UNORM view of its plane reads it and rounded as one writes it, an N-bit sample
// of a 16-bit element being its N most significant bits, as an R10X6 or R12X4 plane holds it. An RGBA pixel is an
// RGBA8, BGRA8 or RGBA16 UNORM texel in 32-bit words.
static void GenBufferAccessFuncs(std::stringstream& shaderStr, VkFormat format, bool isInput)
{
    const char* const bufferName = isInput ? "inputBuffer" : "outputBuffer";
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
    if (mpInfo != nullptr) {
        const uint32_t bits = GetBitsPerChannel(mpInfo->planesLayout);
        const uint32_t shift = (bits > 8) ? (16 - bits) : 0;
        const uint32_t maxValue = (1u << bits) - 1;
        const char* const elementType = (bits > 8) ? "uint16_t" : "uint8_t";
        std::vector<const char*> planes{"Y"};
        if (mpInfo->planesLayout.numberOfExtraPlanes == 1) {
            planes.push_back("CbCr");
        } else if (mpInfo->planesLayout.numberOfExtraPlanes == 2) {
            planes.push_back("Cb");
            planes.push_back("Cr");
        }
        for (const char* plane : planes) {
            if (isInput) {
                shaderStr << "float loadIn" << plane << "(uint index) {\n"
                          << "    return float(uint(" << bufferName << plane << ".data[index]) >> " << shift
                          << ") / " << maxValue << ".0;\n"
                          << "}\n";
            } else {
                shaderStr << "void storeOut" << plane << "(uint index, float value) {\n"
                          << "    " << bufferName << plane << ".data[index] = " << elementType
                          << "(uint(roundEven(clamp(value, 0.0, 1.0) * " << maxValue << ".0)) << " << shift << ");\n"
                          << "}\n";
            }
        }
    } else {
        const char* const swizzle = (format == VK_FORMAT_B8G8R8A8_UNORM) ? ".bgra" : "";
        const bool rgba16 = (RgbaBufferWordsPerPixel(format) == 2);
        if (isInput) {
            shaderStr << "vec4 loadInRGBA(uint index) {\n";
            if (rgba16) {
                shaderStr << "    return vec4(unpackUnorm2x16(inputBufferRGB.data[index]),\n"
                          << "                unpackUnorm2x16(inputBufferRGB.data[index + 1u]));\n";
            } else {
                shaderStr << "    return unpackUnorm4x8(inputBufferRGB.data[index])" << swizzle << ";\n";
            }
            shaderStr << "}\n";
        } else {
            shaderStr << "void storeOutRGBA(uint index, vec4 value) {\n";
            if (rgba16) {
                shaderStr << "    outputBufferRGB.data[index] = packUnorm2x16(value.rg);\n"
                          << "    outputBufferRGB.data[index + 1u] = packUnorm2x16(value.ba);\n";
            } else {
                shaderStr << "    outputBufferRGB.data[index] = packUnorm4x8(value" << swizzle << ");\n";
            }
            shaderStr << "}\n";
        }
    }
    shaderStr << "\n";
}

// GLSL that reads (input) or writes (output) a YCbCr buffer's samples by position, over the element functions
// of GenBufferAccessFuncs(): readInY(), readInCbCr() or readInCb() and readInCr(), and writeOut...() alike. Each
// plane's offset and pitch come from the push constants, in elements. A write outside the output extent, or
// outside its chroma extent, is dropped, as an image store outside the image is: in a buffer it would land in
// the next row or plane.
static void GenYCbCrBufferAccessors(std::stringstream& shaderStr, bool isInput, bool isTwoPlane, bool hasChroma,
                                    uint32_t chromaHorzRatio, uint32_t chromaVertRatio)
{
    const char* const side = isInput ? "in" : "out";
    auto planeIndex = [side](const char* plane, const char* stride) {
        return std::string("pushConstants.") + side + plane + "Offset + uint(p.y) * pushConstants." + side + plane +
               "Pitch + uint(p.x)" + stride;
    };
    const std::string lumaInside = "(uint(p.x) < pushConstants.outputWidth) && (uint(p.y) < pushConstants.outputHeight)";
    const std::string chromaInside =
        "(uint(p.x) < (pushConstants.outputWidth + " + std::to_string(chromaHorzRatio - 1) + "u) / " +
        std::to_string(chromaHorzRatio) + "u) && (uint(p.y) < (pushConstants.outputHeight + " +
        std::to_string(chromaVertRatio - 1) + "u) / " + std::to_string(chromaVertRatio) + "u)";
    if (isInput) {
        shaderStr << "float readInY(ivec2 p) {\n"
                  << "    return loadInY(" << planeIndex("Y", "") << ");\n"
                  << "}\n";
        if (hasChroma && isTwoPlane) {
            shaderStr << "vec2 readInCbCr(ivec2 p) {\n"
                      << "    uint index = " << planeIndex("Cb", " * 2u") << ";\n"
                      << "    return vec2(loadInCbCr(index), loadInCbCr(index + 1u));\n"
                      << "}\n";
        } else if (hasChroma) {
            shaderStr << "float readInCb(ivec2 p) {\n"
                      << "    return loadInCb(" << planeIndex("Cb", "") << ");\n"
                      << "}\n"
                      << "float readInCr(ivec2 p) {\n"
                      << "    return loadInCr(" << planeIndex("Cr", "") << ");\n"
                      << "}\n";
        }
    } else {
        shaderStr << "void writeOutY(ivec2 p, float value) {\n"
                  << "    if (" << lumaInside << ") {\n"
                  << "        storeOutY(" << planeIndex("Y", "") << ", value);\n"
                  << "    }\n"
                  << "}\n";
        if (hasChroma && isTwoPlane) {
            shaderStr << "void writeOutCbCr(ivec2 p, vec2 value) {\n"
                      << "    if (" << chromaInside << ") {\n"
                      << "        uint index = " << planeIndex("Cb", " * 2u") << ";\n"
                      << "        storeOutCbCr(index, value.x);\n"
                      << "        storeOutCbCr(index + 1u, value.y);\n"
                      << "    }\n"
                      << "}\n";
        } else if (hasChroma) {
            shaderStr << "void writeOutCb(ivec2 p, float value) {\n"
                      << "    if (" << chromaInside << ") {\n"
                      << "        storeOutCb(" << planeIndex("Cb", "") << ", value);\n"
                      << "    }\n"
                      << "}\n"
                      << "void writeOutCr(ivec2 p, float value) {\n"
                      << "    if (" << chromaInside << ") {\n"
                      << "        storeOutCr(" << planeIndex("Cr", "") << ", value);\n"
                      << "    }\n"
                      << "}\n";
        }
    }
    shaderStr << "\n";
}

// DEPRECATED -- see the YCBCR2RGBA note in VulkanFilterYuvCompute.h. It serves a 2-plane, a
// 3-plane or a packed 4:4:4 input.
size_t VulkanFilterYuvCompute::InitYCBCR2RGBA(std::string& computeShader)
{
    // The compute filter uses two or three input images as separate planes
    // Y (R) binding = 1
    // 2-planar: CbCr (RG) binding = 2
    // OR
    // 3-planar: Cb (R) binding = 2
    // 3-planar: Cr (R) binding = 3
    // OR
    // packed 4:4:4 (AYUV, Y410, Y416): one image, binding = 0

    // Create compute pipeline
    std::stringstream shaderStr;

    // 1. Generate header and push constants
    GenHeaderAndPushConst(shaderStr, false, m_inputIsBuffer || m_outputIsBuffer);

    // 2. Generate IO bindings
    // Input image
    shaderStr << " // The input YCbCr input binding\n";
    // Input Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   // imageArray: TRUE, and CORRECT -- do not
                                   // "fix" this one to match the output below.
                                   // A multi-planar input binds PLANE views, and
                                   // VkImageResourceView::Create declares plane
                                   // views VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                   // unconditionally. The plane loads below
                                   // index them with ivec3 to match. A packed
                                   // input is the one COLOR binding, which the
                                   // generator declares image2D whatever this
                                   // says, and is loaded with ivec2.
                                   true,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    shaderStr << " // The output RGBA image binding\n";
    // Output Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   // GetOutputFormat gives this filter's output
                                   // the caller's RGBA format, or R8G8B8A8_UNORM
                                   // or R16G16B16A16_UNORM, so
                                   // YcbcrVkFormatInfo is null here and
                                   // ShaderGenerateImagePlaneDescriptors always
                                   // takes the PACKED arm: one COLOR binding,
                                   // bound from GetImageView(). That view is
                                   // VK_IMAGE_VIEW_TYPE_2D for a single-layer
                                   // image and VK_IMAGE_VIEW_TYPE_2D_ARRAY for a
                                   // layered one, so it follows layerCount via
                                   // the caller's FLAG_OUTPUT_IMAGE_ARRAY rather
                                   // than being hardcoded either way -- this
                                   // filter is selectable by VkEncDeriveFilterType
                                   // and is LIVE.
                                   //
                                   // Passing it is belt and braces: the packed
                                   // arm reads OutputDescriptorIsArray() itself,
                                   // because it has to agree with the stores.
                                   OutputDescriptorIsArray(),
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Get format information to determine bit depth
    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo = m_ycbcrConversionCreateInfo;
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(samplerYcbcrConversionCreateInfo.format);

    // The bit depth of the input. A packed 4:4:4 input has no multi-planar description; the
    // packed table gives its depth.
    const uint32_t bitDepth = (m_inputPackedYcbcr != nullptr) ? m_inputPackedYcbcr->bitDepth
                              : (mpInfo != nullptr)           ? GetBitsPerChannel(mpInfo->planesLayout)
                                                              : 8;

    // Determine if we're using limited or full range
    bool isLimitedRange = (samplerYcbcrConversionCreateInfo.ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);

    // 3. Generate helper functions for YCbCr normalization with proper bit depth handling
    GenYCbCrNormalizationFuncs(shaderStr, bitDepth, isLimitedRange, true);
    if (m_inputIsBuffer) {
        GenBufferAccessFuncs(shaderStr, m_inputFormat, true);
    }
    if (m_outputIsBuffer) {
        GenBufferAccessFuncs(shaderStr, m_outputFormat, false);
    }

    // 4. Generate YCbCr to RGB conversion function
    //
    // The range normalization below takes the stored values as UNORM of the input's bit depth:
    // a 10-bit sample of 512 is the chroma zero, 512 / 1023 rather than 128 / 255.
    const unsigned int bpp = bitDepth;

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

    // 6. Main function
    shaderStr <<
        "void main()\n"
        "{\n";
    GenHandleImagePosition(shaderStr);
    GenHandleSourcePositionWithReplicate(shaderStr, m_enableRowAndColumnReplication);
    // Chroma is fetched at the INPUT format's real subsampling ratios, NOT at a
    // hardcoded `srcPos/2`. Halving both axes is right only for 4:2:0: 4:2:2 chroma
    // is full height and only the x axis is halved, and 4:4:4 is not subsampled at
    // all, so a fixed /2 reads chroma from the wrong rows -- a large,
    // chroma-dominated error that leaves luma nearly intact and so reads as a vague
    // "shader generation bug" rather than as a subsampling one. The ratios come from
    // the same YcbcrVkFormatInfo the rest of the plane maths uses, so they cannot
    // drift from the descriptors generated above.
    const uint32_t chromaHorzRatio =
            (mpInfo != nullptr) ? (1u << mpInfo->planesLayout.secondaryPlaneSubsampledX) : 1u;
    const uint32_t chromaVertRatio =
            (mpInfo != nullptr) ? (1u << mpInfo->planesLayout.secondaryPlaneSubsampledY) : 1u;

    if (m_inputPackedYcbcr != nullptr) {
        // A packed input holds the three samples of a pixel in one texel of the image the
        // descriptors above declare as inputImageRGB, each in the channel the format puts it in.
        shaderStr <<
            "    // Fetch the packed " << m_inputPackedYcbcr->debugName << " texel (Y=."
                << PackedChan(m_inputPackedYcbcr->yChannel) << " Cb=."
                << PackedChan(m_inputPackedYcbcr->cbChannel) << " Cr=."
                << PackedChan(m_inputPackedYcbcr->crChannel) << ").\n"
            "    vec4 packedYCbCr = imageLoad(inputImageRGB, srcPos);\n"
            "    float Y = packedYCbCr." << PackedChan(m_inputPackedYcbcr->yChannel) << ";\n"
            "    vec2 CbCr = vec2(packedYCbCr." << PackedChan(m_inputPackedYcbcr->cbChannel)
                << ", packedYCbCr." << PackedChan(m_inputPackedYcbcr->crChannel) << ");\n";
    } else {
        if (m_inputIsBuffer) {
            shaderStr <<
                "    // Fetch from the buffer.\n"
                "    float Y = loadInY(pushConstants.inYOffset + uint(srcPos.y) * pushConstants.inYPitch + uint(srcPos.x));\n";
        } else {
            shaderStr <<
                "    // Fetch from the texture.\n"
                "    float Y = imageLoad(inputImageY, ivec3(srcPos, pushConstants.srcLayer)).r;\n";
        }
        shaderStr <<
            "    // Chroma subsampling of the input format: "
                << chromaHorzRatio << "x" << chromaVertRatio << "\n"
            "    ivec2 chromaPos = ivec2(srcPos.x / " << chromaHorzRatio
                << ", srcPos.y / " << chromaVertRatio << ");\n";
        // Cb and Cr from the planes the descriptors above declare: one interleaved plane for a
        // 2-plane input, one plane each for a 3-plane input.
        if (m_inputIsBuffer && (mpInfo != nullptr) && (mpInfo->planesLayout.numberOfExtraPlanes == 2)) {
            shaderStr <<
                "    vec2 CbCr = vec2(loadInCb(pushConstants.inCbOffset + uint(chromaPos.y) * pushConstants.inCbPitch + uint(chromaPos.x)),\n"
                "                     loadInCr(pushConstants.inCrOffset + uint(chromaPos.y) * pushConstants.inCrPitch + uint(chromaPos.x)));\n";
        } else if (m_inputIsBuffer) {
            shaderStr <<
                "    uint cbcrIndex = pushConstants.inCbOffset + uint(chromaPos.y) * pushConstants.inCbPitch + uint(chromaPos.x) * 2u;\n"
                "    vec2 CbCr = vec2(loadInCbCr(cbcrIndex), loadInCbCr(cbcrIndex + 1u));\n";
        } else if ((mpInfo != nullptr) && (mpInfo->planesLayout.numberOfExtraPlanes == 2)) {
            shaderStr <<
                "    vec2 CbCr = vec2(imageLoad(inputImageCb, ivec3(chromaPos, pushConstants.srcLayer)).r,\n"
                "                     imageLoad(inputImageCr, ivec3(chromaPos, pushConstants.srcLayer)).r);\n";
        } else {
            shaderStr <<
                "    vec2 CbCr = imageLoad(inputImageCbCr, ivec3(chromaPos, pushConstants.srcLayer)).rg;\n";
        }
    }
    shaderStr <<
        "\n"
        // normalizeYCbCr() maps the range and centers Cb and Cr on zero: it is the
        // whole step from stored values to the matrix's input.
        "    vec3 ycbcr = normalizeYCbCr(vec3(Y, CbCr));\n"
        "    vec4 rgba = vec4(convertYCbCrToRgb(ycbcr),1.0);\n"
        "    // Store it back.\n"
        "    // The index form follows the binding declared above.\n";
    if (m_outputIsBuffer) {
        shaderStr <<
            "    storeOutRGBA(pushConstants.outYOffset + uint(pos.y) * pushConstants.outYPitch + uint(pos.x) * "
                << RgbaBufferWordsPerPixel(m_outputFormat) << "u, rgba);\n"
            "}\n";
    } else {
        shaderStr <<
            "    imageStore(outputImageRGB, "
                << OutStoreIndex("pos", OutputDescriptorIsArray()) << ", rgba);\n"
            "}\n";
    }

    computeShader = shaderStr.str();
    if (dumpShaders)
        VkEncOut() << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

static uint32_t GetFormatBitDepth(VkFormat format, uint32_t enableMsbToLsbShift)
{
    // A packed alias reports its real component depth. Without this
    // A2B10G10R10_UNORM_PACK32 is described as 4 channels / 4 bytes and comes out as
    // 8-bit, which picks the wrong black level and the wrong AQ subsampled-Y format.
    const VkPackedYcbcrFormatInfo* packedInfo = PackedYcbcrFormatInfo(format);
    if (packedInfo != nullptr) {
        return packedInfo->bitDepth;
    }

    const VkFormatDesc* pFormatInfo = vkFormatLookUp(format);
    uint32_t bitDepth = (pFormatInfo != nullptr) ? (8 * pFormatInfo->numberOfBytes / pFormatInfo->numberOfChannels) : 8;

    if (enableMsbToLsbShift != 0) {
        // The API required shift but the format is not explicitly not specified
        if (bitDepth == 16) {
            bitDepth = 10;
        }
    }
    return bitDepth;
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

    // Get format information to determine bit depths and chroma subsampling
    const VkMpFormatInfo* inputMpInfo = YcbcrVkFormatInfo(m_inputFormat);
    const VkMpFormatInfo* outputMpInfo = YcbcrVkFormatInfo(m_outputFormat);

    // Determine bit depth from the formats
    const uint32_t inputBitDepth = inputMpInfo ? GetBitsPerChannel(inputMpInfo->planesLayout)    : GetFormatBitDepth(m_inputFormat,  m_inputEnableMsbToLsbShift);
    const uint32_t outputBitDepth = outputMpInfo ? GetBitsPerChannel(outputMpInfo->planesLayout) : GetFormatBitDepth(m_outputFormat, m_outputEnableLsbToMsbShift);

    // Get input/output chroma subsampling ratios
    const uint32_t inputChromaHorzRatio = (inputMpInfo != nullptr) ? (1 << inputMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t inputChromaVertRatio = (inputMpInfo != nullptr) ? (1 << inputMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1;
    const uint32_t outputChromaHorzRatio = (outputMpInfo != nullptr) ? (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t outputChromaVertRatio = (outputMpInfo != nullptr) ? (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1;

    // Determine if we're using limited or full range for input and output
    // Default to limited range as it's more common for YCbCr content
    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo = m_ycbcrConversionCreateInfo;
    const bool isInputLimitedRange = (samplerYcbcrConversionCreateInfo.ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
    const bool isOutputLimitedRange = isInputLimitedRange; // Usually same as input, but could be configurable

    // Check if input or output are buffers
    const bool isInputBuffer = m_inputIsBuffer;
    const bool isOutputBuffer = m_outputIsBuffer;

    // Check if we need to do any range conversion
    const bool needsRangeConversion = (isInputLimitedRange != isOutputLimitedRange);

    std::stringstream shaderStr;

    // 1. Generate header and push constants
    GenHeaderAndPushConst(shaderStr, false, isInputBuffer || isOutputBuffer);

    // 2. Generate IO bindings
    //
    // imageArray is TRUE on both sides here and reads as unconditional, but it
    // is a request about the PLANE bindings only: either side of this filter
    // may be packed single-plane (AYUV / Y410), and
    // ShaderGenerateImagePlaneDescriptors ignores this argument on that arm.
    // The input side is image2D by construction; the output side comes from
    // OutputDescriptorIsArray(), which is false here because this generator's
    // callers do not set FLAG_OUTPUT_IMAGE_ARRAY -- so the arm still declares
    // image2D, matching the combined VK_IMAGE_VIEW_TYPE_2D view it binds.
    // Do not re-derive packed-ness here to pass a different flag -- that
    // duplicate is what let the declaration and the binding disagree.
    //
    // Input Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   true, // isInput
                                   0,    // startBinding
                                   0,    // set
                                   true, // imageArray (plane bindings)
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Output Descriptors
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   true,  // imageArray (plane bindings)
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // Binding 9: Subsampled Y output image (optional, only if enabled)
    if (m_enableYSubsampling) {
        VkEncOut() << "[VulkanFilterYuvCompute] Generating binding 9 for subsampledImageY" << std::endl;
        shaderStr << "// Binding 9: Subsampled Y output (2x2 downsampled luma for AQ)\n";
        // Use a separate variable for subsampledImage aspects to avoid modifying m_outputImageAspects
        // ShaderGenerateImagePlaneDescriptors modifies the imageAspects parameter for R8/R16 formats
        VkImageAspectFlags subsampledImageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT; // Y-only
        ShaderGenerateImagePlaneDescriptors(shaderStr,
                                            subsampledImageAspects,
                                            "subsampledImage",
                                            (outputBitDepth > 8) ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
                                            false,       // isInput
                                            8,           // startBinding
                                            0,           // set
                                            false        // imageArray - use simple image2D, not image2DArray
                                            );
    }

    shaderStr << "\n";

    // Determine input and output plane configurations
    // A packed side carries its chroma inside the single COLOR_BIT image, so it has no
    // PLANE_1/PLANE_2 aspect bits. Without the extra term the read path substitutes
    // neutral chroma (cb = cr = 128/255) and the conversion silently produces greyscale.
    const bool hasInputChroma = ((m_inputImageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) ||
                                (m_inputPackedYcbcr != nullptr);
    const bool hasOutputChroma = ((m_outputImageAspects & (VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0) ||
                                 (m_outputPackedYcbcr != nullptr);

    // Determine if input is two-plane (e.g., NV12) or three-plane (e.g., I420)
    const bool isInputTwoPlane = (m_inputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) &&
                          !(m_inputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT);

    // Determine if output is two-plane (e.g., NV12) or three-plane (e.g., I420)
    const bool isOutputTwoPlane = (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) &&
                           !(m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT);

    // 3. A buffer side loads and stores its samples normalized, as an UNORM view of its plane does
    //    (GenBufferAccessFuncs), by position (GenYCbCrBufferAccessors). So the copy runs in the
    //    normalized [0,1] domain on every side: the loads and stores do the bit-depth scaling, and
    //    only a RANGE change needs any code at all.
    if (isInputBuffer) {
        GenBufferAccessFuncs(shaderStr, m_inputFormat, true);
        GenYCbCrBufferAccessors(shaderStr, true, isInputTwoPlane, hasInputChroma,
                                inputChromaHorzRatio, inputChromaVertRatio);
    }
    if (isOutputBuffer) {
        GenBufferAccessFuncs(shaderStr, m_outputFormat, false);
        GenYCbCrBufferAccessors(shaderStr, false, isOutputTwoPlane, hasOutputChroma,
                                outputChromaHorzRatio, outputChromaVertRatio);
    }
    const bool needsConversion = needsRangeConversion;

    // 4. Helper function for the conversion, if one is needed at all
    if (needsConversion) {
        GenConvertYCbCrRangeNormalized(shaderStr, inputBitDepth, isInputLimitedRange,
                                       outputBitDepth, isOutputLimitedRange,
                                       hasInputChroma || hasOutputChroma);
    }

    // 5. Main function
    shaderStr <<
        "void main()\n"
        "{\n";

    // Luma pixels per invocation. This MUST match the CmdDispatch() grid, which is
    // ceil(outputSize / m_blockHorz|VertRatio) in RecordCommandBuffer(). A block size
    // that disagrees with the grid fails silently: a 2x2 block under a 4:2:2 grid of
    // (w/2, h) puts chromaPos at lumaPos/2, so chroma rows only ever reach h/2 of a
    // plane that is h tall and the bottom half is never written, and a 4:4:4 grid
    // dispatches 4x the threads it needs.
    //
    // Adaptive Quantization needs a fixed 2x2 box filter for its subsampled-Y image, so
    // when that is on the block stays 2x2 and the chroma write loop emits the
    // (block / chromaRatio) samples the larger block then covers.
    m_blockHorzRatio = m_enableYSubsampling ? 2 : outputChromaHorzRatio;
    m_blockVertRatio = m_enableYSubsampling ? 2 : outputChromaVertRatio;
    const uint32_t ySubsampleHorzRatio = m_blockHorzRatio;
    const uint32_t ySubsampleVertRatio = m_blockVertRatio;

    // 6. Generate block coordinates (always 2x2 for optimal dispatch)
    GenBlockCoordinates(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio,
                        m_enableRowAndColumnReplication,
                        inputChromaHorzRatio, inputChromaVertRatio);

    // 7. Read YCbCr block (always read 2x2 Y block, but chroma depends on input format)
    GenReadYCbCrBlock(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio, isInputTwoPlane, hasInputChroma, m_enableRowAndColumnReplication,
                      inputChromaHorzRatio, inputChromaVertRatio, m_inputPackedYcbcr, isInputBuffer);

    // 7b. Undo the source's MSB alignment BEFORE anything reads the values.
    //      Pairs with step 9; see GenApplyBlockInputShift for why the output
    //      half alone saturates an X6/X4-packed source.
    GenApplyBlockInputShift(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio, inputBitDepth, m_inputEnableMsbToLsbShift, hasInputChroma);

    // 8. Convert block (if needed)
    // cbcrOut is DECLARED here but REFERENCED by GenWriteYCbCrBlock, which gates on
    // hasOutputChroma. Gating the declaration on hasInputChroma alone leaves it undeclared
    // whenever a chroma-less input feeds a chroma output (packed input, or a Y-only R8/R16
    // source) -- a second 'undeclared identifier' right behind the first.
    GenConvertYCbCrBlock(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio, needsConversion, hasInputChroma || hasOutputChroma);

    // 9. Apply output bit shifts (if needed)
    GenApplyBlockOutputShift(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio, outputBitDepth, m_outputEnableLsbToMsbShift, hasOutputChroma);

    // 10. Write YCbCr block (handles both 4:2:0 and 4:4:4 output)
    GenWriteYCbCrBlock(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio, isOutputTwoPlane, hasOutputChroma,
                       outputChromaHorzRatio, outputChromaVertRatio, m_outputPackedYcbcr, OutputDescriptorIsArray(),
                       isOutputBuffer);

    // 11. Compute and write subsampled Y (only if enabled for AQ).
    // The box filter spans the invocation's luma block, which is 2x2 for the
    // 4:2:0 output that is the only configuration enabling this: the encoder
    // sets FLAG_ENABLE_Y_SUBSAMPLING only alongside its NV12 encode input
    // (VkVideoEncoder.cpp). A non-4:2:0 output would size the "subsampled" Y
    // differently; that combination has no caller and is not addressed here.
    if (m_enableYSubsampling) {
        GenComputeAndWriteSubsampledY(shaderStr, ySubsampleHorzRatio, ySubsampleVertRatio);
    }

    // Close main function
    shaderStr << "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        VkEncOut() << "\nCompute Shader:\n" << computeShader;
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


    // 3. Main function - OPTIMIZED: One thread clears one chroma-sized block
    shaderStr <<
        "void main()\n"
        "{\n";

    shaderStr <<
        "    // OPTIMIZED: Dispatch at chroma resolution (" <<
        (chromaHorzRatio == 2 ? (chromaVertRatio == 2 ? "4:2:0" : "4:2:2") : "4:4:4") << ")\n"
        "    // Each thread clears " << chromaHorzRatio << "x" << chromaVertRatio << " Y pixels + 1 CbCr pixel\n"
        "    ivec2 chromaPos = ivec2(gl_GlobalInvocationID.xy);\n"
        "    \n"
        "    // Calculate chroma output dimensions\n"
        "    uint chromaWidth = (pushConstants.outputWidth + " << (chromaHorzRatio-1) << ") / " << chromaHorzRatio << ";\n"
        "    uint chromaHeight = (pushConstants.outputHeight + " << (chromaVertRatio-1) << ") / " << chromaVertRatio << ";\n"
        "    \n"
        "    // Bounds check at chroma resolution\n"
        "    if ((chromaPos.x >= chromaWidth) || (chromaPos.y >= chromaHeight)) {\n"
        "        return;\n"
        "    }\n"
        "    \n"
        "    // Calculate top-left luma position for this " << chromaHorzRatio << "x" << chromaVertRatio << " block\n"
        "    ivec2 lumaPos = chromaPos * ivec2(" << chromaHorzRatio << ", " << chromaVertRatio << ");\n"
        "    \n";

    // Clear Y pixels in the block
    shaderStr << "    // Clear " << chromaHorzRatio << "x" << chromaVertRatio << " Y pixels (50% intensity)\n";
    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            shaderStr <<
                "    imageStore(outputImageY, ivec3(lumaPos + ivec2(" << x << ", " << y << "), pushConstants.dstLayer), "
                "vec4(0.5, 0, 0, 1));\n";
        }
    }

    shaderStr << "    \n";

    // Clear chroma planes
    if (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_1_BIT) {
        shaderStr << "    // Clear 1 CbCr pixel (2-plane, 50% intensity = middle range)\n"
                  << "    imageStore(outputImageCbCr, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0.5, 0, 1));\n";
    }

    if (m_outputImageAspects & VK_IMAGE_ASPECT_PLANE_2_BIT) {
        shaderStr << "    // Clear 1 Cb and 1 Cr pixel (3-plane, 50% intensity = middle range)\n"
                  << "    imageStore(outputImageCb, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0, 0, 1));\n"
                  << "    imageStore(outputImageCr, ivec3(chromaPos, pushConstants.dstLayer), vec4(0.5, 0, 0, 1));\n";
    }

    shaderStr << "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        VkEncOut() << "\nCompute Shader:\n" << computeShader;
    return computeShader.size();
}

/**
 * @brief Generates GLSL function for RGB to YCbCr color space conversion
 *
 * Converts normalized RGB [0,1] to Y'CbCr with Y in [0,1] and Cb/Cr in [-0.5, 0.5],
 * and folds the requested output range into the same function.
 *
 * The range is applied here, as one vec3 fma on the result, rather than as a separate
 * per-pixel pass over the block: it is the last step of the conversion, so keeping it
 * inside gives the compiler one expression to fold instead of a scale-and-offset chain
 * spread across N statements. It cannot be folded any further back -- into the matrix
 * coefficients -- because cb and cr are derived from the *unscaled* y.
 *
 * Full range emits nothing: the [0,1] result is already what the UNORM store wants, so
 * the identity scale-by-one/offset-by-zero never reaches the shader.
 *
 * Applying the range at all is not optional. The write sites hand these values to
 * imageStore() on a UNORM image, which scales by the format's full-scale maximum, so
 * anything that skips this step answers an ITU_NARROW request with full-range content.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param btStandard The color primaries standard (BT.601, BT.709, BT.2020)
 * @param bitDepth Output bit depth, which sets the narrow-range code levels
 * @param isLimitedRange Whether the output carries ITU narrow-range levels
 */
static void GenRgbToYCbCrConversion(std::stringstream& shaderStr,
                                     YcbcrBtStandard btStandard,
                                     uint32_t bitDepth,
                                     bool isLimitedRange)
{
    const YcbcrPrimariesConstants primariesConstants = GetYcbcrPrimariesConstants(btStandard);
    const YcbcrRangeConstants rangeConstants = GetYcbcrRangeConstants(YcbcrLevelsDigital);
    const YcbcrBtMatrix yCbCrMatrix(primariesConstants.kb,
                                    primariesConstants.kr,
                                    rangeConstants.cbMax,
                                    rangeConstants.crMax);

    shaderStr <<
        "// RGB to YCbCr conversion\n"
        "// Input: normalized RGB [0,1]\n"
        "// Output: Y in [0,1], Cb/Cr in [-0.5, 0.5]"
        << (isLimitedRange ? ", scaled to narrow range\n" : " (full range)\n") <<
        "vec3 convertRgbToYCbCr(vec3 rgb) {\n"
        "    float y, cb, cr;\n"
        "    float r = rgb.r;\n"
        "    float g = rgb.g;\n"
        "    float b = rgb.b;\n";

    yCbCrMatrix.ConvertRgbToYCbCrDiscreteChString(shaderStr, "    ");

    if (!isLimitedRange) {
        shaderStr <<
            "    return vec3(y, cb, cr);\n"
            "}\n\n";
        return;
    }

    // store = Y * (yRange/max) + yBlack/max, and
    // store = (C + 0.5) * (cScale/max) + cZero/max for chroma. The write sites add the
    // 0.5 themselves, so what is folded in here is the residual
    // ((cZero + cScale/2) / max) - 0.5.
    const YCbCrRangeLevels levels = GetYCbCrRangeLevels(bitDepth, true);
    const double yScale  = (levels.yWhite - levels.yBlack) / levels.maxValue;
    const double yOffset = levels.yBlack / levels.maxValue;
    const double cScale  = levels.cScale / levels.maxValue;
    const double cOffset = ((levels.cZero + (levels.cScale / 2.0)) / levels.maxValue) - 0.5;

    std::stringstream ss;
    ss.precision(16);
    ss << std::fixed;

    ss << "    // " << bitDepth << "-bit ITU narrow range: Y[" << (uint32_t)levels.yBlack
       << ", " << (uint32_t)levels.yWhite << "], Cb/Cr[" << (uint32_t)levels.cZero
       << ", " << (uint32_t)(levels.cZero + levels.cScale) << "]\n"
       << "    return vec3(y, cb, cr) * vec3(" << yScale << ", " << cScale << ", " << cScale
       << ") + vec3(" << yOffset << ", " << cOffset << ", " << cOffset << ");\n";

    shaderStr << ss.str() << "}\n\n";
}

/**
 * @brief Generates GLSL code for reading a block of RGB pixels from input image
 *
 * Creates code to read NxM RGB pixels for block-based processing.
 * Handles edge replication if enabled.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal block size (1 for 4:4:4, 2 for 4:2:0/4:2:2)
 * @param chromaVertRatio Vertical block size (1 for 4:4:4/4:2:2, 2 for 4:2:0)
 * @param enableReplication Whether to enable edge pixel replication
 */
static void GenReadRgbBlock(std::stringstream& shaderStr,
                            uint32_t chromaHorzRatio,
                            uint32_t chromaVertRatio,
                            bool enableReplication,
                            uint32_t bufferWordsPerPixel = 0)  // an RGBA buffer input (loadInRGBA) when not 0
{
    if (bufferWordsPerPixel != 0) {
        shaderStr << "    // Read " << chromaHorzRatio << "x" << chromaVertRatio << " RGB block (RGBA buffer)\n";
        for (uint32_t y = 0; y < chromaVertRatio; y++) {
            for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                shaderStr << "    ivec2 rgbPos" << x << y << " = ";
                if (enableReplication) {
                    shaderStr << "min(lumaPos + ivec2(" << x << ", " << y << "), inputLumaMax);\n";
                } else {
                    shaderStr << "lumaPos + ivec2(" << x << ", " << y << ");\n";
                }
                shaderStr << "    vec3 rgb" << x << y << " = loadInRGBA(pushConstants.inYOffset + uint(rgbPos" << x << y
                          << ".y) * pushConstants.inYPitch + uint(rgbPos" << x << y << ".x) * "
                          << bufferWordsPerPixel << "u).rgb;\n";
            }
        }
        shaderStr << "    \n";
        return;
    }

    shaderStr << "    // Read " << chromaHorzRatio << "x" << chromaVertRatio
              << " RGB block (imageLoad, STORAGE_IMAGE)\n";

    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            // imageLoad returns the vec4 in RGBA order regardless of the byte
            // order the VkFormat names -- that is a property of the format, not
            // of the access -- so .rgb is the correct swizzle; the declaration
            // names the view's format, or none for one SPIR-V has no format for.
            shaderStr << "    vec3 rgb" << x << y << " = "
                      << "imageLoad"
                      << "(inputImageRGBA, ";
            if (enableReplication) {
                shaderStr << "min(lumaPos + ivec2(" << x << ", " << y << "), inputLumaMax)";
            } else {
                shaderStr << "lumaPos + ivec2(" << x << ", " << y << ")";
            }
            shaderStr << ").rgb;\n";
        }
    }
    shaderStr << "    \n";
}

/**
 * @brief Generates GLSL code for converting a block of RGB pixels to YCbCr
 *
 * Creates code to convert each RGB pixel in the block to YCbCr.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 */
static void GenConvertRgbBlockToYCbCr(std::stringstream& shaderStr,
                                       uint32_t chromaHorzRatio,
                                       uint32_t chromaVertRatio)
{
    shaderStr << "    // Convert RGB to YCbCr\n";

    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            shaderStr << "    vec3 ycbcr" << x << y
                      << " = convertRgbToYCbCr(rgb" << x << y << ");\n";
        }
    }
    shaderStr << "    \n";
}

/**
 * @brief Generates GLSL code for averaging chroma values in a block
 *
 * Creates code to compute the average Cb and Cr values for chroma downsampling.
 * Uses simple box filter (average of all samples in the block).
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 */
static void GenAverageChromaBlock(std::stringstream& shaderStr,
                                   uint32_t chromaHorzRatio,
                                   uint32_t chromaVertRatio)
{
    // For 4:4:4 (1x1), no averaging needed
    if (chromaHorzRatio == 1 && chromaVertRatio == 1) {
        shaderStr << "    // 4:4:4 format - no chroma averaging\n"
                  << "    vec2 avgCbCr = ycbcr00.yz;\n"
                  << "    \n";
        return;
    }

    float scale = 1.0f / (chromaHorzRatio * chromaVertRatio);

    shaderStr << "    // Average CbCr for " << chromaHorzRatio << "x"
              << chromaVertRatio << " block (box filter)\n"
              << "    vec2 avgCbCr = (";

    bool first = true;
    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            if (!first) shaderStr << " + ";
            shaderStr << "ycbcr" << x << y << ".yz";
            first = false;
        }
    }

    shaderStr << ") * " << scale << ";\n    \n";
}

/**
 * @brief Generates GLSL code for writing Y values from a block
 *
 * Creates code to write all Y values in a block to the Y plane.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param chromaHorzRatio Horizontal block size
 * @param chromaVertRatio Vertical block size
 */
static void GenWriteYBlock(std::stringstream& shaderStr,
                           uint32_t chromaHorzRatio,
                           uint32_t chromaVertRatio,
                           bool outputIsBuffer = false)
{
    if (outputIsBuffer) {
        // A buffer has no bounds a store is checked against, as an image has: a position past the output's
        // edge is skipped here.
        shaderStr << "    // Write " << chromaHorzRatio << "x" << chromaVertRatio << " Y samples (buffer)\n";
        for (uint32_t y = 0; y < chromaVertRatio; y++) {
            for (uint32_t x = 0; x < chromaHorzRatio; x++) {
                shaderStr << "    if ((uint(lumaPos.x) + " << x << "u < pushConstants.outputWidth) && "
                          << "(uint(lumaPos.y) + " << y << "u < pushConstants.outputHeight)) {\n"
                          << "        storeOutY(pushConstants.outYOffset + (uint(lumaPos.y) + " << y
                          << "u) * pushConstants.outYPitch + uint(lumaPos.x) + " << x << "u, ycbcr" << x << y << ".x);\n"
                          << "    }\n";
            }
        }
        shaderStr << "    \n";
        return;
    }

    shaderStr << "    // Write " << chromaHorzRatio << "x" << chromaVertRatio << " Y pixels\n";

    for (uint32_t y = 0; y < chromaVertRatio; y++) {
        for (uint32_t x = 0; x < chromaHorzRatio; x++) {
            // Y value is in [0, 1], no need to add 0.5
            shaderStr <<
                "    imageStore(outputImageY, ivec3(lumaPos + ivec2(" << x << ", " << y
                << "), pushConstants.dstLayer), vec4(ycbcr" << x << y << ".x, 0, 0, 1));\n";
        }
    }
    shaderStr << "    \n";
}

/**
 * @brief Generates GLSL code for writing chroma values
 *
 * Creates code to write averaged CbCr values to chroma plane(s).
 * Handles 2-plane (NV12-style) and 3-plane (I420-style) formats.
 * CbCr values are shifted from [-0.5, 0.5] back to [0, 1] for storage.
 *
 * @param shaderStr Output stringstream where the GLSL code will be written
 * @param isOutputTwoPlane Whether output is 2-plane format
 * @param chromaHorzRatio Horizontal subsampling ratio
 * @param chromaVertRatio Vertical subsampling ratio
 */
static void GenWriteChromaBlock(std::stringstream& shaderStr,
                                 bool isOutputTwoPlane,
                                 uint32_t chromaHorzRatio,
                                 uint32_t chromaVertRatio,
                                 bool outputIsBuffer = false)
{
    if (outputIsBuffer) {
        // As the image variant: the chroma of the block at its chroma position, of each pixel for 4:4:4;
        // Cb and Cr shifted from [-0.5, 0.5] to [0, 1].
        const bool is444Buffer = (chromaHorzRatio == 1) && (chromaVertRatio == 1);
        const char* const pos = is444Buffer ? "lumaPos" : "chromaPos";
        const char* const value = is444Buffer ? "ycbcr00.yz" : "avgCbCr";
        shaderStr << "    // Write Cb and Cr (buffer)\n"
                  << "    vec2 cbcrOut = " << value << " + vec2(0.5);\n";
        if (isOutputTwoPlane) {
            shaderStr << "    uint cbcrOutIndex = pushConstants.outCbOffset + uint(" << pos << ".y) * pushConstants.outCbPitch + uint("
                      << pos << ".x) * 2u;\n"
                      << "    storeOutCbCr(cbcrOutIndex, cbcrOut.x);\n"
                      << "    storeOutCbCr(cbcrOutIndex + 1u, cbcrOut.y);\n";
        } else {
            shaderStr << "    storeOutCb(pushConstants.outCbOffset + uint(" << pos << ".y) * pushConstants.outCbPitch + uint("
                      << pos << ".x), cbcrOut.x);\n"
                      << "    storeOutCr(pushConstants.outCrOffset + uint(" << pos << ".y) * pushConstants.outCrPitch + uint("
                      << pos << ".x), cbcrOut.y);\n";
        }
        shaderStr << "    \n";
        return;
    }

    // For 4:4:4 format, write chroma at each luma position
    bool is444 = (chromaHorzRatio == 1 && chromaVertRatio == 1);

    if (is444) {
        // 4:4:4 format - write chroma at every pixel
        if (isOutputTwoPlane) {
            shaderStr << "    // Write CbCr at luma position (4:4:4, 2-plane format)\n"
                      << "    // Shift CbCr from [-0.5, 0.5] to [0, 1] for storage\n"
                      << "    imageStore(outputImageCbCr, ivec3(lumaPos, pushConstants.dstLayer), "
                      << "vec4(ycbcr00.y + 0.5, ycbcr00.z + 0.5, 0, 1));\n";
        } else {
            shaderStr << "    // Write Cb and Cr at luma position (4:4:4, 3-plane format)\n"
                      << "    // Shift CbCr from [-0.5, 0.5] to [0, 1] for storage\n"
                      << "    imageStore(outputImageCb, ivec3(lumaPos, pushConstants.dstLayer), "
                      << "vec4(ycbcr00.y + 0.5, 0, 0, 1));\n"
                      << "    imageStore(outputImageCr, ivec3(lumaPos, pushConstants.dstLayer), "
                      << "vec4(ycbcr00.z + 0.5, 0, 0, 1));\n";
        }
    } else {
        // Subsampled format - write averaged chroma
        if (isOutputTwoPlane) {
            shaderStr << "    // Write CbCr (2-plane format)\n"
                      << "    // Shift CbCr from [-0.5, 0.5] to [0, 1] for storage\n"
                      << "    imageStore(outputImageCbCr, ivec3(chromaPos, pushConstants.dstLayer), "
                      << "vec4(avgCbCr.x + 0.5, avgCbCr.y + 0.5, 0, 1));\n";
        } else {
            shaderStr << "    // Write Cb and Cr (3-plane format)\n"
                      << "    // Shift CbCr from [-0.5, 0.5] to [0, 1] for storage\n"
                      << "    imageStore(outputImageCb, ivec3(chromaPos, pushConstants.dstLayer), "
                      << "vec4(avgCbCr.x + 0.5, 0, 0, 1));\n"
                      << "    imageStore(outputImageCr, ivec3(chromaPos, pushConstants.dstLayer), "
                      << "vec4(avgCbCr.y + 0.5, 0, 0, 1));\n";
        }
    }
    shaderStr << "    \n";
}

size_t VulkanFilterYuvCompute::InitRGBA2YCBCR(std::string& computeShader)
{
    // For RGBA2YCBCR filter, input is a single RGBA image (VK_IMAGE_ASPECT_COLOR_BIT)
    m_inputImageAspects = VK_IMAGE_ASPECT_COLOR_BIT;

    // Get output format information to determine subsampling and bit depth
    const VkMpFormatInfo* outputMpInfo = YcbcrVkFormatInfo(m_outputFormat);

    // Determine output chroma subsampling ratios
    // For 4:2:0: chromaHorzRatio=2, chromaVertRatio=2
    // For 4:2:2: chromaHorzRatio=2, chromaVertRatio=1
    // For 4:4:4: chromaHorzRatio=1, chromaVertRatio=1
    const uint32_t chromaHorzRatio = outputMpInfo ?
        (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledX) : 1;
    const uint32_t chromaVertRatio = outputMpInfo ?
        (1 << outputMpInfo->planesLayout.secondaryPlaneSubsampledY) : 1;

    // Determine output bit depth
    const uint32_t outputBitDepth = outputMpInfo ?
        GetBitsPerChannel(outputMpInfo->planesLayout) : GetFormatBitDepth(m_outputFormat, m_outputEnableLsbToMsbShift);

    // Determine if output is 2-plane (NV12-style) or 3-plane (I420-style)
    const bool isOutputTwoPlane = outputMpInfo ?
        (outputMpInfo->planesLayout.numberOfExtraPlanes == 1) : false;

    // Get color space configuration
    const VkSamplerYcbcrConversionCreateInfo& samplerYcbcrConversionCreateInfo = m_ycbcrConversionCreateInfo;
    const bool isLimitedRange = (samplerYcbcrConversionCreateInfo.ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
    const YcbcrBtStandard btStandard = GetYcbcrPrimariesConstantsId(samplerYcbcrConversionCreateInfo.ycbcrModel);

    std::stringstream shaderStr;

    // 1. Generate header and push constants; an input with no image format is read without one
    GenHeaderAndPushConst(shaderStr, !m_inputIsBuffer && (GlslImageFormatQualifier(m_inputFormat)[0] == '\0'),
                          m_inputIsBuffer || m_outputIsBuffer);

    // 2. Input RGBA image binding (binding 0)
    //
    // THE COLOUR CONTRACT, stated here because it is decided by the pair
    // (view format, access form) and by nothing else:
    //
    //   * The view must be a _UNORM view, never a *_SRGB one, and two
    //     independent facts say so.
    //
    //     WHY _UNORM. This filter applies the colour MATRIX ONLY --
    //     GenRgbToYCbCrConversion emits the Rec.601/709/2020 matrix and an
    //     optional range compression, and no transfer function anywhere
    //     (see also ycbcr_utils.h, "No gamma correction yet"). That is
    //     CORRECT, because Rec.709/601 Y'CbCr is defined on GAMMA-ENCODED
    //     R'G'B'. The _UNORM view hands the stored code values over
    //     untouched, which is what the matrix wants.
    //
    //     WHY A *_SRGB VIEW CANNOT REACH THIS BINDING AT ALL. The binding
    //     declared below is a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE read with
    //     imageLoad, and no *_SRGB format carries
    //     VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT -- sRGB is a sampled-image
    //     feature. A view whose usage includes VK_IMAGE_USAGE_STORAGE_BIT
    //     must carry that feature itself
    //     (VUID-VkImageViewCreateInfo-usage-02275), so an sRGB view can
    //     never become this descriptor. A storage read applies no transfer
    //     function of its own either, which is why the requirement above is
    //     about the CODE VALUES the view holds and not about the fetch.
    //   * This is a property of the VIEW, so it is the caller's to honour;
    //     what is guaranteed here is that nothing on this path introduces a
    //     transfer function of its own. A _UNORM view in gives R'G'B' in.
    //
    // The component order is a property of the VIEW. The declaration names the
    // input's own format (GlslImageFormatQualifier): `rgba8` for R8G8B8A8_UNORM,
    // and none for B8G8R8A8_UNORM, which SPIR-V has no image format for, so it
    // is read without one and imageLoad returns its components in RGBA order.
    if (m_inputIsBuffer) {
        // An RGBA buffer, binding 0 (inputBufferRGB).
        ShaderGeneratePlaneDescriptors(shaderStr,
                                       true,  // isInput
                                       0,     // startBinding
                                       0,     // set
                                       false,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    } else {
        shaderStr << " // Input RGBA image binding\n";
        GenImageIoBindingLayout(shaderStr, "inputImage", "RGBA", GlslImageFormatQualifier(m_inputFormat), true, 0, 0, false);
    }

    // 3. Output YCbCr plane bindings (bindings 4-7)
    // This will set m_outputImageAspects based on the output format
    ShaderGeneratePlaneDescriptors(shaderStr,
                                   false, // isInput
                                   4,     // startBinding
                                   0,     // set
                                   // imageArray follows the OUTPUT IMAGE TYPE, which is what
                                   // decides which view the descriptor binds:
                                   //   multi-planar -> per-plane views, always
                                   //                   VK_IMAGE_VIEW_TYPE_2D_ARRAY -> image2DArray
                                   //   packed        -> SINGLE-plane, so there is no per-plane
                                   //                   view; it binds the combined view, which
                                   //                   follows layerCount and is 2D -> image2D
                                   // Hardcoding true breaks every packed 4:4:4 format (AYUV, Y410): the
                                   // generated store then fails to compile, and forcing that store to ivec3
                                   // instead trips the VUID from the other direction -- "view is 2D but
                                   // OpTypeImage Arrayed = 1". (m_outputPackedYcbcr == nullptr) gets the
                                   // planar/packed half of that right but assumes the packed arm is
                                   // single-layer; OutputDescriptorIsArray() carries both halves.
                                   OutputDescriptorIsArray(),
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    // 3b. Binding 9: Subsampled Y output image (optional, only if enabled)
    if (m_enableYSubsampling) {
        shaderStr << "\n// Binding 9: Subsampled Y output (2x2 downsampled luma for AQ)\n";
        VkImageAspectFlags subsampledImageAspects = VK_IMAGE_ASPECT_PLANE_0_BIT;
        ShaderGenerateImagePlaneDescriptors(shaderStr,
                                            subsampledImageAspects,
                                            "subsampledImage",
                                            (outputBitDepth > 8) ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
                                            false,       // isInput
                                            8,           // startBinding (becomes binding 9 after ++startBinding)
                                            0,           // set
                                            false        // imageArray - use simple image2D
                                            );
    }

    shaderStr << "\n";

    if (m_inputIsBuffer) {
        GenBufferAccessFuncs(shaderStr, m_inputFormat, true);
    }
    if (m_outputIsBuffer) {
        GenBufferAccessFuncs(shaderStr, m_outputFormat, false);
    }

    // 4. Generate RGB to YCbCr conversion function
    GenRgbToYCbCrConversion(shaderStr, btStandard, outputBitDepth, isLimitedRange);

    // 5. Precision qualifiers. The output range is applied in the normalized domain, at
    // step 9b.
    shaderStr << "\n"
              << "// Specify high precision for all floating point calculations\n"
              << "precision highp float;\n"
              << "precision highp int;\n"
              << "\n";

    // 6. Main function
    shaderStr <<
        "void main()\n"
        "{\n";

    // 7. Generate block coordinates for chroma-resolution dispatch
    // For 4:4:4, dispatch at full resolution; for 4:2:0, dispatch at half resolution
    if (chromaHorzRatio > 1 || chromaVertRatio > 1) {
        // RGBA input has no chroma plane, so srcChromaPos is unused here; pass the
        // block ratio to keep the emitted expression well-formed.
        GenBlockCoordinates(shaderStr, chromaHorzRatio, chromaVertRatio,
                            m_enableRowAndColumnReplication,
                            chromaHorzRatio, chromaVertRatio);
    } else {
        // 4:4:4 format - dispatch at full resolution
        shaderStr <<
            "    // 4:4:4 format - one thread per pixel\n"
            "    ivec2 lumaPos = ivec2(gl_GlobalInvocationID.xy);\n"
            "    \n"
            "    // Bounds check at luma resolution\n"
            "    if ((lumaPos.x >= pushConstants.outputWidth) || (lumaPos.y >= pushConstants.outputHeight)) {\n"
            "        return;\n"
            "    }\n"
            "    \n";

        if (m_enableRowAndColumnReplication) {
            shaderStr <<
                "    // Input bounds for edge replication\n"
                "    ivec2 inputLumaMax = ivec2(pushConstants.inputWidth - 1, pushConstants.inputHeight - 1);\n"
                "    \n";
        }
    }

    // 8. Read RGB block
    GenReadRgbBlock(shaderStr, chromaHorzRatio, chromaVertRatio,
                    m_enableRowAndColumnReplication,
                    m_inputIsBuffer ? RgbaBufferWordsPerPixel(m_inputFormat) : 0);

    // 9. Convert RGB to YCbCr
    GenConvertRgbBlockToYCbCr(shaderStr, chromaHorzRatio, chromaVertRatio);

    // 10. Average chroma for downsampling (if needed)
    GenAverageChromaBlock(shaderStr, chromaHorzRatio, chromaVertRatio);

    // 11-12. Write output: packed or planar
    const bool isPackedOutput = (outputMpInfo == nullptr);
    if (isPackedOutput) {
        // Single-plane packed YCbCr written through one RGBA-typed storage image.
        // ycbcr00 = (Y, Cb, Cr); Cb/Cr are shifted from [-0.5,0.5] to [0,1] for
        // UNORM storage. The channel order depends on the *target* packed
        // format's in-memory byte order (the storage image is a generic RGBA
        // format whose channels we repurpose). Packed 4:2:2 (YUY2/Y210/Y216) is not
        // handled here: it stores two luma per texel at half width, so it needs a
        // macropixel-aware dispatch.
        //
        // The per-format channel order comes from packedYcbcrFormatInfo[] (the single
        // table the read path, GetFormatBitDepth() and the encoder-side selection all
        // share) rather than from a switch duplicated here. A duplicated channel order
        // is a transposed Cb/Cr waiting to happen, and a transposed Cb/Cr is a plausible
        // picture rather than an error.
        if (m_outputPackedYcbcr == nullptr) {
            assert(!"Packed output without a packed format descriptor");
            computeShader.clear();
            return 0;   // Init() treats a zero-length shader as a failure.
        }

        // ycbcr00 = (Y, Cb, Cr). Place each one at the channel the target format
        // carries it in; the 4th channel is opaque alpha.
        const char* component[4] = { "1.0", "1.0", "1.0", "1.0" };
        component[m_outputPackedYcbcr->yChannel]  = "ycbcr00.x";
        component[m_outputPackedYcbcr->cbChannel] = "ycbcr00.y + 0.5";
        component[m_outputPackedYcbcr->crChannel] = "ycbcr00.z + 0.5";

        shaderStr << "    // Write packed " << m_outputPackedYcbcr->debugName
                  << " (Y=." << PackedChan(m_outputPackedYcbcr->yChannel)
                  << " Cb=."  << PackedChan(m_outputPackedYcbcr->cbChannel)
                  << " Cr=."  << PackedChan(m_outputPackedYcbcr->crChannel) << ", A=1)\n"
                  // 2-component on purpose: a packed format is SINGLE-plane, so this
                  // binding is not a per-plane view. It is bound from the combined view,
                  // which follows layerCount and is VK_IMAGE_VIEW_TYPE_2D -- hence the
                  // matching image2D declaration in InitRGBA2YCBCR. The planar branch
                  // below is the one that needs dstLayer.
                  << "    imageStore(outputImageRGB, "
                  << OutStoreIndex("lumaPos", OutputDescriptorIsArray()) << ", vec4("
                  << component[0] << ", " << component[1] << ", "
                  << component[2] << ", " << component[3] << "));\n"
                  << "    \n";
    } else {
        // Multi-planar output: separate Y and chroma plane writes
        GenWriteYBlock(shaderStr, chromaHorzRatio, chromaVertRatio, m_outputIsBuffer);
        GenWriteChromaBlock(shaderStr, isOutputTwoPlane, chromaHorzRatio, chromaVertRatio, m_outputIsBuffer);
    }

    // 13. Compute and write subsampled Y (only if enabled for AQ)
    if (m_enableYSubsampling) {
        // For RGBA2YCBCR, we always dispatch at chroma resolution (half for 4:2:0)
        // The Y values are in ycbcr00.x, ycbcr10.x, ycbcr01.x, ycbcr11.x
        shaderStr <<
            "    // Write 2x2 subsampled Y for Adaptive Quantization\n"
            "    // chromaPos is at half resolution, write averaged Y\n"
            "    float subsampledY = 0.25 * (ycbcr00.x + ycbcr10.x + ycbcr01.x + ycbcr11.x);\n"
            "    imageStore(subsampledImageY, chromaPos, vec4(subsampledY, 0, 0, 1));\n"
            "    \n";
    }

    // Close main function
    shaderStr << "}\n";

    computeShader = shaderStr.str();
    if (dumpShaders)
        VkEncOut() << "\nRGBA2YCBCR Compute Shader:\n" << computeShader;
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

            // The buffer from its start to the plane's end: the shader adds the plane's offset (the push
            // constants), and a descriptor at the plane's own offset would have to meet
            // minStorageBufferOffsetAlignment, which tightly packed planes do not.
            const VkSubresourceLayout& planeLayout = vkBufferSubresourceLayout[planeNum];
            bufferDescriptors[descrIndex].buffer = vkBuffers[bufferIndex];
            bufferDescriptors[descrIndex].offset = 0;
            bufferDescriptors[descrIndex].range = planeLayout.offset +
                                                  ((planeLayout.size != 0) ? planeLayout.size : planeLayout.arrayPitch);
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

    // Trim plane aspects to match the view's actual plane count.
    // The default m_inputImageAspects/m_outputImageAspects include all 3 plane bits,
    // but 2-plane formats (NV12, P010, etc.) only have PLANE_0 and PLANE_1.
    uint32_t numPlanes = imageView->GetNumberOfPlanes();
    if (numPlanes < 3) validImageAspects &= ~VK_IMAGE_ASPECT_PLANE_2_BIT;
    if (numPlanes < 2) validImageAspects &= ~VK_IMAGE_ASPECT_PLANE_1_BIT;
    if (numPlanes < 1) validImageAspects &= ~VK_IMAGE_ASPECT_PLANE_0_BIT;

    uint32_t curImageAspect = 0;
    while(validImageAspects) {

        if (validImageAspects & (VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect) ) {

            VkSampler ccSampler = (curImageAspect == 0) ? convSampler : VK_NULL_HANDLE;
            uint32_t planeNum = GetPlaneIndex((VkImageAspectFlagBits)(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect));

            // For packed formats (e.g. A2B10G10R10 / Y410), curImageAspect==0 uses the
            // combined view (GetImageView()), so we don't need separate plane views.
            // Only assert plane count for multi-planar plane access (curImageAspect > 0).
            if (curImageAspect > 0) {
                assert(planeNum < numPlanes);
            }

            uint32_t dstBinding = baseBinding;
            if (curImageAspect > 0) {
                // the first plane is 1, second plane is 2, the 3rd is 3
                dstBinding += (1 + planeNum);
            }

            writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
            writeDescriptorSets[descrIndex].dstBinding = dstBinding;
            writeDescriptorSets[descrIndex].descriptorCount = 1;
            const VkDescriptorType effectiveDescriptorType =
                (ccSampler != VK_NULL_HANDLE) ?
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                    descriptorType;
            writeDescriptorSets[descrIndex].descriptorType = effectiveDescriptorType;
            imageDescriptors[descrIndex].sampler = ccSampler;
            imageDescriptors[descrIndex].imageView = (curImageAspect == 0) ?
                                                      imageView->GetImageView() :
                                                      imageView->GetPlaneImageView(planeNum);
            assert(imageDescriptors[descrIndex].imageView);
            // A storage-image descriptor is only valid in VK_IMAGE_LAYOUT_GENERAL
            // (VUID-VkDescriptorImageInfo-imageView-06711). The caller passes ONE layout
            // for the whole side of the filter, and for a YCbCr input that layout is the
            // sampled-image layout -- which would silently be applied to the per-plane
            // storage descriptors as well. Decide per descriptor instead.
            imageDescriptors[descrIndex].imageLayout =
                (writeDescriptorSets[descrIndex].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ?
                    VK_IMAGE_LAYOUT_GENERAL : imageLayout;
            writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex]; // Y (0) plane
            descrIndex++;
            validImageAspects &= ~(VK_IMAGE_ASPECT_COLOR_BIT << curImageAspect);
        }

        curImageAspect++;
    }
    assert(descrIndex <= maxDescriptors);
    return descrIndex;
}


// Image input -> Image output + optional subsampled Y output (overload for AQ)
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     uint32_t bufferIdx,
                                                     const VkImageResourceView* inImageView,
                                                     const VkVideoPictureResourceInfoKHR * inImageResourceInfo,
                                                     const VkImageResourceView* outImageView,
                                                     const VkVideoPictureResourceInfoKHR * outImageResourceInfo,
                                                     const VkImageResourceView* subsampledImageView,
                                                     const VkVideoPictureResourceInfoKHR* subsampledImageResourceInfo)
{

    assert(cmdBuf != VK_NULL_HANDLE);
    
    // Ensure consistency: if subsampledImageView is provided, m_enableYSubsampling must be true
    // The enableYSubsampling flag must be set to true in Create() before Init() is called
    if (subsampledImageView != nullptr && !m_enableYSubsampling) {
        assert(!"subsampledImageView provided but m_enableYSubsampling is false. Pass enableYSubsampling=true to Create()");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

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

            // IN 0: an RGBA or packed 4:4:4 YCbCr input (AYUV / Y410)
            // IN 1: y plane - G -> R8
            // IN 2: Cb or Cr or CbCr plane - BR -> R8B8
            // IN 3: Cr or Cb plane - R -> R8
            // Each is a storage image the shader reads with imageLoad(), with no sampler
            // (see InitDescriptorSetLayout).
            // GENERAL is valid for both sampled and storage reads, the filter never
            // transitions its input to SHADER_READ_ONLY_OPTIMAL itself, and no caller hands
            // it an input in that layout: the decoder hands over a VIDEO_DECODE_DPB_KHR image
            // that it moves to GENERAL, and the encoder hands over a GENERAL/TRANSFER_SRC
            // linear staging image. Declaring SHADER_READ_ONLY_OPTIMAL here would not
            // match either of them.
            const VkImageLayout inputLayout = VK_IMAGE_LAYOUT_GENERAL;
            UpdateImageDescriptorSets(inImageView,
                                      m_inputImageAspects,
                                      VK_NULL_HANDLE, // no sampler
                                      inputLayout,
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

            dstBinding = 9;
            // Binding 9: Optional subsampled Y output image (for AQ)
            if (subsampledImageView != nullptr) {
                writeDescriptorSets[descrIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writeDescriptorSets[descrIndex].dstSet = VK_NULL_HANDLE;
                writeDescriptorSets[descrIndex].dstBinding = dstBinding;
                writeDescriptorSets[descrIndex].descriptorCount = 1;
                writeDescriptorSets[descrIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                imageDescriptors[descrIndex].sampler = VK_NULL_HANDLE;
                imageDescriptors[descrIndex].imageView = subsampledImageView->GetImageView();
                imageDescriptors[descrIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                writeDescriptorSets[descrIndex].pImageInfo = &imageDescriptors[descrIndex];
                descrIndex++;
            }

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
        ivec2    halfInputSize;    // Precomputed (inputSize + 1) / 2
        ivec2    halfOutputSize;   // Precomputed (outputSize + 1) / 2
        uint32_t yOffset;   // Y plane offset
        uint32_t cbOffset;  // Cb plane offset
        uint32_t crOffset;  // Cr plane offset
        uint32_t yPitch;    // Y plane pitch
        uint32_t cbPitch;   // Cb plane pitch
        uint32_t crPitch;   // Cr plane pitch
    };

    uint32_t inputWidth = inImageResourceInfo->codedExtent.width;
    uint32_t inputHeight = inImageResourceInfo->codedExtent.height;
    uint32_t outputWidth = outImageResourceInfo->codedExtent.width;
    uint32_t outputHeight = outImageResourceInfo->codedExtent.height;

    PushConstants pushConstants = {
            inImageResourceInfo->baseArrayLayer, // Set the source layer index
            outImageResourceInfo->baseArrayLayer, // Set the destination layer index
            ivec2(inputWidth, inputHeight),
            ivec2(outputWidth, outputHeight),
            ivec2((inputWidth + 1) / 2, (inputHeight + 1) / 2),    // halfInputSize
            ivec2((outputWidth + 1) / 2, (outputHeight + 1) / 2),  // halfOutputSize
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

    // Dispatch based on output format's chroma subsampling
    // 4:2:0 (NV12, P010, I420): dispatch at (width/2, height/2) - each thread handles 2x2 luma
    // 4:2:2 (NV16, P210): dispatch at (width/2, height) - each thread handles 2x1 luma  
    // 4:4:4 (YUV444): dispatch at (width, height) - each thread handles 1x1 luma
    // Packed formats (e.g. Y410 / A2B10G10R10): outputMpInfo is null but it's 4:4:4 → ratio 1
    // Use the SAME block ratio the shader was generated with (set in Init(), possibly
    // overridden by the per-type Init), not a freshly recomputed output chroma ratio.
    // The two diverge whenever Adaptive Quantization forces a 2x2 block, and any
    // divergence silently leaves part of the output unwritten or dispatches threads
    // that write out of bounds.
    const uint32_t dispatchWidth = (pushConstants.outputSize.width + m_blockHorzRatio - 1) / m_blockHorzRatio;
    const uint32_t dispatchHeight = (pushConstants.outputSize.height + m_blockVertRatio - 1) / m_blockVertRatio;

    const uint32_t workgroupWidth = (dispatchWidth + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t workgroupHeight = (dispatchHeight + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    // Image barrier: make compute shader writes to the output image visible
    // to other queues (graphics for display, host for dump).
    // NOTE: The caller (decoder) may add additional image-specific barriers
    // (e.g., HOST_READ_BIT for LINEAR images used by CPU dump).
    {
        VkImageMemoryBarrier2 imgBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        imgBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        imgBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        imgBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        imgBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        imgBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.image = outImageView->GetImageResource()->GetImage();
        imgBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarrier.subresourceRange.baseMipLevel = 0;
        imgBarrier.subresourceRange.levelCount = 1;
        imgBarrier.subresourceRange.baseArrayLayer = 0;
        imgBarrier.subresourceRange.layerCount = 1;
        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &imgBarrier;
        m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &depInfo);
    }

    return VK_SUCCESS;
}

// The push constants and the dispatch of a buffer RecordCommandBuffer() overload. A buffer side gives each
// plane's offset and row pitch in bytes (its subresource layouts); its shader takes them in the elements it
// declares for the plane (GenBufferAccessFuncs): a byte per 8-bit YCbCr sample, 16 bits per deeper one, a 32-bit
// word per RGBA pixel (two for RGBA16). The dispatch covers the output's extent with the block the shader was
// generated for, as the image overload's does.
VkResult VulkanFilterYuvCompute::RecordBufferDispatch(VkCommandBuffer cmdBuf,
                                                      const VkExtent2D& inputExtent, uint32_t srcLayer,
                                                      const VkSubresourceLayout* inLayouts, uint32_t numInPlanes,
                                                      const VkExtent2D& outputExtent, uint32_t dstLayer,
                                                      const VkSubresourceLayout* outLayouts, uint32_t numOutPlanes)
{
    auto toElements = [](VkFormat format, const VkSubresourceLayout* layouts, uint32_t numPlanes,
                         uint32_t offsets[3], uint32_t pitches[3]) -> bool {
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
        const VkDeviceSize elementSize =
            (mpInfo == nullptr) ? 4 : ((GetBitsPerChannel(mpInfo->planesLayout) > 8) ? 2 : 1);
        for (uint32_t i = 0; (i < numPlanes) && (i < 3); i++) {
            if (((layouts[i].offset % elementSize) != 0) || ((layouts[i].rowPitch % elementSize) != 0)) {
                return false;
            }
            offsets[i] = static_cast<uint32_t>(layouts[i].offset / elementSize);
            pitches[i] = static_cast<uint32_t>(layouts[i].rowPitch / elementSize);
        }
        return true;
    };
    uint32_t inOffsets[3] = {};
    uint32_t inPitches[3] = {};
    uint32_t outOffsets[3] = {};
    uint32_t outPitches[3] = {};
    if (((inLayouts != nullptr) && !toElements(m_inputFormat, inLayouts, numInPlanes, inOffsets, inPitches)) ||
        ((outLayouts != nullptr) && !toElements(m_outputFormat, outLayouts, numOutPlanes, outOffsets, outPitches))) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    PushConstants pushConstants = {};
    pushConstants.srcLayer = srcLayer;
    pushConstants.dstLayer = dstLayer;
    pushConstants.inputWidth = inputExtent.width;
    pushConstants.inputHeight = inputExtent.height;
    pushConstants.outputWidth = outputExtent.width;
    pushConstants.outputHeight = outputExtent.height;
    pushConstants.halfInputWidth = (inputExtent.width + 1) / 2;
    pushConstants.halfInputHeight = (inputExtent.height + 1) / 2;
    pushConstants.halfOutputWidth = (outputExtent.width + 1) / 2;
    pushConstants.halfOutputHeight = (outputExtent.height + 1) / 2;
    pushConstants.inYOffset = inOffsets[0];
    pushConstants.inCbOffset = inOffsets[1];
    pushConstants.inCrOffset = inOffsets[2];
    pushConstants.inYPitch = inPitches[0];
    pushConstants.inCbPitch = inPitches[1];
    pushConstants.inCrPitch = inPitches[2];
    pushConstants.outYOffset = outOffsets[0];
    pushConstants.outCbOffset = outOffsets[1];
    pushConstants.outCrOffset = outOffsets[2];
    pushConstants.outYPitch = outPitches[0];
    pushConstants.outCbPitch = outPitches[1];
    pushConstants.outCrPitch = outPitches[2];
    m_vkDevCtx->CmdPushConstants(cmdBuf,
                                 m_descriptorSetLayout.GetPipelineLayout(),
                                 VK_SHADER_STAGE_COMPUTE_BIT,
                                 0,
                                 sizeof(PushConstants),
                                 &pushConstants);

    const uint32_t dispatchWidth = (outputExtent.width + m_blockHorzRatio - 1) / m_blockHorzRatio;
    const uint32_t dispatchHeight = (outputExtent.height + m_blockVertRatio - 1) / m_blockVertRatio;
    const uint32_t workgroupWidth = (dispatchWidth + (m_workgroupSizeX - 1)) / m_workgroupSizeX;
    const uint32_t workgroupHeight = (dispatchHeight + (m_workgroupSizeY - 1)) / m_workgroupSizeY;
    m_vkDevCtx->CmdDispatch(cmdBuf, workgroupWidth, workgroupHeight, 1);

    // The shader's writes, to a buffer or an image, made visible to what reads the output next, as the image
    // overload does for its output image.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &barrier;
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &depInfo);
    return VK_SUCCESS;
}

// Buffer input -> Image output
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     uint32_t bufferIdx,
                                                     const VkBuffer*            inBuffers,
                                                     uint32_t                   numInBuffers,
                                                     const VkFormat*            inBufferFormats,
                                                     const VkSubresourceLayout* inBufferSubresourceLayouts,
                                                     uint32_t                   inBufferNumPlanes,
                                                     const VkImageResourceView* outImageView,
                                                     const VkVideoPictureResourceInfoKHR* outImageResourceInfo,
                                                     const VkBufferImageCopy* pBufferImageCopy,
                                                     const VkImageResourceView* subsampledYImageView,
                                                     const VkVideoPictureResourceInfoKHR* subsampledImageResourceInfo)
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

    // The input's extent: the region's, or the output's when the region gives none.
    const VkExtent2D inputExtent = ((pBufferImageCopy != nullptr) && (pBufferImageCopy->imageExtent.width != 0))
                                       ? VkExtent2D{pBufferImageCopy->imageExtent.width,
                                                    pBufferImageCopy->imageExtent.height}
                                       : outImageResourceInfo->codedExtent;
    return RecordBufferDispatch(cmdBuf,
                                inputExtent, 0, inBufferSubresourceLayouts, inBufferNumPlanes,
                                outImageResourceInfo->codedExtent, outImageResourceInfo->baseArrayLayer,
                                nullptr, 0);
}

// Image input -> Buffer output
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     uint32_t bufferIdx,
                                                     const VkImageResourceView* inImageView,
                                                     const VkVideoPictureResourceInfoKHR* inImageResourceInfo,
                                                     const VkBuffer*            outBuffers,        // with size numOutBuffers
                                                     uint32_t                   numOutBuffers,
                                                     const VkFormat*            outBufferFormats,   // with size outBufferNumPlanes
                                                     const VkSubresourceLayout* outBufferSubresourceLayouts, // with size outBufferNumPlanes
                                                     uint32_t                   outBufferNumPlanes,
                                                     const VkBufferImageCopy*   pBufferImageCopy,
                                                     const VkImageResourceView* subsampledYImageView,
                                                     const VkVideoPictureResourceInfoKHR* subsampledImageResourceInfo)
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

            // IN 0: an RGBA or packed 4:4:4 YCbCr input (AYUV / Y410)
            // IN 1: y plane - G -> R8
            // IN 2: Cb or Cr or CbCr plane - BR -> R8B8
            // IN 3: Cr or Cb plane - R -> R8
            // Each is a storage image the shader reads with imageLoad(), with no sampler
            // (see InitDescriptorSetLayout).
            // GENERAL is valid for both sampled and storage reads, the filter never
            // transitions its input to SHADER_READ_ONLY_OPTIMAL itself, and no caller hands
            // it an input in that layout: the decoder hands over a VIDEO_DECODE_DPB_KHR image
            // that it moves to GENERAL, and the encoder hands over a GENERAL/TRANSFER_SRC
            // linear staging image. Declaring SHADER_READ_ONLY_OPTIMAL here would not
            // match either of them.
            const VkImageLayout inputLayout = VK_IMAGE_LAYOUT_GENERAL;
            UpdateImageDescriptorSets(inImageView,
                                      m_inputImageAspects,
                                      VK_NULL_HANDLE, // no sampler
                                      inputLayout,
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
                                       m_outputImageAspects,
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

    // The output's extent: the region's, or the input's when the region gives none.
    const VkExtent2D outputExtent = ((pBufferImageCopy != nullptr) && (pBufferImageCopy->imageExtent.width != 0))
                                        ? VkExtent2D{pBufferImageCopy->imageExtent.width,
                                                     pBufferImageCopy->imageExtent.height}
                                        : inImageResourceInfo->codedExtent;
    return RecordBufferDispatch(cmdBuf,
                                inImageResourceInfo->codedExtent, inImageResourceInfo->baseArrayLayer, nullptr, 0,
                                outputExtent, 0, outBufferSubresourceLayouts, outBufferNumPlanes);
}

// Buffer input -> Buffer output (all buffer case)
VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     uint32_t bufferIdx,
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
                                                     const VkImageResourceView* subsampledYImageView,
                                                     const VkVideoPictureResourceInfoKHR* subsampledImageResourceInfo)
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
                                       m_outputImageAspects,
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

    return RecordBufferDispatch(cmdBuf,
                                VkExtent2D{inBufferExtent.width, inBufferExtent.height}, 0,
                                inBufferSubresourceLayouts, numInPlanes,
                                VkExtent2D{outBufferExtent.width, outBufferExtent.height}, 0,
                                outBufferSubresourceLayouts, numOutPlanes);
}

// =============================================================================
// Transfer Operations Implementation
// =============================================================================

VkResult VulkanFilterYuvCompute::CreateTransfer(const VulkanDeviceContext* vkDevCtx,
                                                uint32_t queueFamilyIndex,
                                                uint32_t queueIndex,
                                                TransferOpType transferType,
                                                VkSharedBaseObj<VulkanFilter>& vulkanFilter) {
    // Map TransferOpType to FilterType
    FilterType filterType;
    switch (transferType) {
        case TransferOpType::COPY_IMAGE_TO_BUFFER:
            filterType = XFER_IMAGE_TO_BUFFER;
            break;
        case TransferOpType::COPY_BUFFER_TO_IMAGE:
            filterType = XFER_BUFFER_TO_IMAGE;
            break;
        case TransferOpType::COPY_IMAGE_TO_IMAGE:
            filterType = XFER_IMAGE_TO_IMAGE;
            break;
        default:
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Create the filter with transfer-only mode
    VkSharedBaseObj<VulkanFilterYuvCompute> yuvComputeFilter(
        new VulkanFilterYuvCompute(vkDevCtx, queueFamilyIndex, queueIndex,
                                   filterType, 1,  // maxNumFrames = 1 for transfer-only
                                   VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                   FLAG_SKIP_COMPUTE, nullptr));
    
    if (!yuvComputeFilter) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    
    vulkanFilter = yuvComputeFilter;
    return VK_SUCCESS;
}

void VulkanFilterYuvCompute::RecordLayoutTransition(VkCommandBuffer cmdBuf,
                                                    const TransferResource& resource,
                                                    VkImageLayout newLayout,
                                                    VkPipelineStageFlags srcStage,
                                                    VkPipelineStageFlags dstStage,
                                                    VkAccessFlags srcAccess,
                                                    VkAccessFlags dstAccess) {
    if (!resource.isImage() || resource.image == VK_NULL_HANDLE) {
        return;
    }
    
    if (resource.currentLayout == newLayout) {
        return;
    }
    
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = resource.currentLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource.image;
    barrier.subresourceRange.aspectMask = resource.aspectMask;
    barrier.subresourceRange.baseMipLevel = resource.mipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = resource.arrayLayer;
    barrier.subresourceRange.layerCount = 1;
    
    m_vkDevCtx->CmdPipelineBarrier(cmdBuf, srcStage, dstStage, 0,
                                   0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanFilterYuvCompute::RecordPreTransfers(VkCommandBuffer cmdBuf,
                                               const FilterExecutionDesc& execDesc) {
    for (uint32_t i = 0; i < execDesc.numInputs; ++i) {
        const FilterIOSlot& slot = execDesc.inputs[i];
        if (!slot.hasPreTransfer()) {
            continue;
        }
        
        // Transition source to TRANSFER_SRC_OPTIMAL
        TransferResource srcCopy = slot.preTransferSource;
        RecordLayoutTransition(cmdBuf, srcCopy, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, VK_ACCESS_TRANSFER_READ_BIT);
        
        // Transition destination (primary) to TRANSFER_DST_OPTIMAL
        TransferResource dstCopy = slot.primary;
        RecordLayoutTransition(cmdBuf, dstCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, VK_ACCESS_TRANSFER_WRITE_BIT);
        
        // Record the transfer operation
        RecordTransferOp(cmdBuf, slot.preTransferOp, slot.preTransferSource, slot.primary);
    }
    
    // Barrier: TRANSFER → COMPUTE (if there were any pre-transfers)
    if (execDesc.hasAnyPreTransfer()) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(cmdBuf,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}

void VulkanFilterYuvCompute::RecordPostTransfers(VkCommandBuffer cmdBuf,
                                                const FilterExecutionDesc& execDesc) {
    // Barrier: COMPUTE → TRANSFER (if there are any post-transfers)
    if (execDesc.hasAnyPostTransfer()) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(cmdBuf,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    
    for (uint32_t i = 0; i < execDesc.numOutputs; ++i) {
        const FilterIOSlot& slot = execDesc.outputs[i];
        if (!slot.hasPostTransfer()) {
            continue;
        }
        
        // Transition source (primary) to TRANSFER_SRC_OPTIMAL
        TransferResource srcCopy = slot.primary;
        RecordLayoutTransition(cmdBuf, srcCopy, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        
        // Transition destination to TRANSFER_DST_OPTIMAL (if it's an image)
        TransferResource dstCopy = slot.postTransferDest;
        if (dstCopy.isImage()) {
            RecordLayoutTransition(cmdBuf, dstCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        
        // Record the transfer operation
        RecordTransferOp(cmdBuf, slot.postTransferOp, slot.primary, slot.postTransferDest);
    }
    
    // Final barrier for host readback (if any post-transfer destination is a buffer or linear image)
    bool needsHostBarrier = false;
    for (uint32_t i = 0; i < execDesc.numOutputs; ++i) {
        const FilterIOSlot& slot = execDesc.outputs[i];
        if (slot.hasPostTransfer()) {
            if (slot.postTransferDest.isBuffer() || slot.postTransferDest.isLinear()) {
                needsHostBarrier = true;
                break;
            }
        }
    }
    
    if (needsHostBarrier) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(cmdBuf,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_HOST_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}

void VulkanFilterYuvCompute::RecordTransferOp(VkCommandBuffer cmdBuf,
                                             TransferOpType opType,
                                             const TransferResource& src,
                                             const TransferResource& dst) {
    switch (opType) {
        case TransferOpType::COPY_IMAGE_TO_BUFFER: {
            if (!src.isImage() || !dst.isBuffer()) {
                return;
            }
            
            VkBufferImageCopy regions[3];
            uint32_t numRegions = 0;
            CalculateBufferImageCopyRegions(src, dst, regions, numRegions);
            
            m_vkDevCtx->CmdCopyImageToBuffer(cmdBuf, src.image,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             dst.buffer, numRegions, regions);
            break;
        }
        
        case TransferOpType::COPY_BUFFER_TO_IMAGE: {
            if (!src.isBuffer() || !dst.isImage()) {
                return;
            }
            
            VkBufferImageCopy regions[3];
            uint32_t numRegions = 0;
            CalculateBufferImageCopyRegions(dst, src, regions, numRegions);
            
            m_vkDevCtx->CmdCopyBufferToImage(cmdBuf, src.buffer,
                                             dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             numRegions, regions);
            break;
        }
        
        case TransferOpType::COPY_IMAGE_TO_IMAGE: {
            if (!src.isImage() || !dst.isImage()) {
                return;
            }
            
            VkImageCopy regions[3];
            uint32_t numRegions = 0;
            CalculateImageCopyRegions(src, dst, regions, numRegions);
            
            m_vkDevCtx->CmdCopyImage(cmdBuf, src.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     numRegions, regions);
            break;
        }
        
        default:
            break;
    }
}

void VulkanFilterYuvCompute::CalculateBufferImageCopyRegions(const TransferResource& image,
                                                             const TransferResource& buffer,
                                                             VkBufferImageCopy regions[3],
                                                             uint32_t& numRegions) {
    numRegions = 0;
    
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(image.format);
    
    if (!mpInfo || mpInfo->planesLayout.numberOfExtraPlanes == 0) {
        // Single-plane format
        VkBufferImageCopy& region = regions[numRegions++];
        region = {};
        
        // Use explicit buffer geometry if available
        if (buffer.numPlanes >= 1 && buffer.planeGeometry[0].width > 0) {
            const PlaneBufferGeometry& geom = buffer.planeGeometry[0];
            region.bufferOffset = geom.offset;
            // bufferRowLength is in texels, not bytes. 0 = tightly packed (width).
            // If rowPitch differs from width * bytesPerPixel, calculate texel count.
            if (geom.rowPitch > 0 && geom.bytesPerPixel > 0) {
                uint32_t texelsPerRow = static_cast<uint32_t>(geom.rowPitch / geom.bytesPerPixel);
                region.bufferRowLength = (texelsPerRow != geom.width) ? texelsPerRow : 0;
            } else {
                region.bufferRowLength = 0;  // Tightly packed
            }
            region.bufferImageHeight = 0;  // Assumed tightly packed in height
        } else {
            // Fallback: use legacy fields if planeGeometry not set
            region.bufferOffset = buffer.getPlaneOffset(0);
            // Without bytesPerPixel info, can't compute texel width from pitch
            // buffer.getPlaneRowPitch(0) returns pitch but we can't use it
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
        }
        
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = image.mipLevel;
        region.imageSubresource.baseArrayLayer = image.arrayLayer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = image.extent;
    } else {
        // Multi-plane format
        uint32_t numPlanes = mpInfo->planesLayout.numberOfExtraPlanes + 1;
        
        for (uint32_t plane = 0; plane < numPlanes && numRegions < 3; ++plane) {
            VkBufferImageCopy& region = regions[numRegions++];
            region = {};
            
            // Use explicit buffer geometry for this plane
            if (plane < buffer.numPlanes && buffer.planeGeometry[plane].width > 0) {
                const PlaneBufferGeometry& geom = buffer.planeGeometry[plane];
                region.bufferOffset = geom.offset;
                
                // bufferRowLength is in texels. Convert from byte pitch.
                if (geom.rowPitch > 0 && geom.bytesPerPixel > 0) {
                    uint32_t texelsPerRow = static_cast<uint32_t>(geom.rowPitch / geom.bytesPerPixel);
                    region.bufferRowLength = (texelsPerRow != geom.width) ? texelsPerRow : 0;
                } else {
                    region.bufferRowLength = 0;
                }
                region.bufferImageHeight = 0;
                
                // Use geometry's extent for this plane
                region.imageExtent.width = geom.width;
                region.imageExtent.height = geom.height;
                region.imageExtent.depth = 1;
            } else {
                // Fallback: compute from image format
                region.bufferOffset = buffer.getPlaneOffset(plane);
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                
                // Calculate plane extent based on subsampling
                if (plane == 0) {
                    region.imageExtent = image.extent;
                } else {
                    uint32_t subsampledX = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledX;
                    uint32_t subsampledY = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledY;
                    region.imageExtent.width = (image.extent.width + subsampledX - 1) / subsampledX;
                    region.imageExtent.height = (image.extent.height + subsampledY - 1) / subsampledY;
                    region.imageExtent.depth = 1;
                }
            }
            
            // Set aspect mask based on plane
            if (plane == 0) {
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            } else if (plane == 1) {
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            } else {
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
            }
            
            region.imageSubresource.mipLevel = image.mipLevel;
            region.imageSubresource.baseArrayLayer = image.arrayLayer;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
        }
    }
}

void VulkanFilterYuvCompute::CalculateImageCopyRegions(const TransferResource& srcImage,
                                                       const TransferResource& dstImage,
                                                       VkImageCopy regions[3],
                                                       uint32_t& numRegions) {
    numRegions = 0;
    
    // Use source format to determine planes
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(srcImage.format);
    
    if (!mpInfo || mpInfo->planesLayout.numberOfExtraPlanes == 0) {
        // Single-plane format
        VkImageCopy& region = regions[numRegions++];
        region = {};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel = srcImage.mipLevel;
        region.srcSubresource.baseArrayLayer = srcImage.arrayLayer;
        region.srcSubresource.layerCount = 1;
        region.srcOffset = {0, 0, 0};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.mipLevel = dstImage.mipLevel;
        region.dstSubresource.baseArrayLayer = dstImage.arrayLayer;
        region.dstSubresource.layerCount = 1;
        region.dstOffset = {0, 0, 0};
        region.extent = srcImage.extent;
    } else {
        // Multi-plane format
        uint32_t numPlanes = mpInfo->planesLayout.numberOfExtraPlanes + 1;
        
        for (uint32_t plane = 0; plane < numPlanes && numRegions < 3; ++plane) {
            VkImageCopy& region = regions[numRegions++];
            region = {};
            
            // Set aspect mask based on plane
            VkImageAspectFlags planeAspect;
            if (plane == 0) {
                planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
            } else if (plane == 1) {
                planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
            } else {
                planeAspect = VK_IMAGE_ASPECT_PLANE_2_BIT;
            }
            
            region.srcSubresource.aspectMask = planeAspect;
            region.srcSubresource.mipLevel = srcImage.mipLevel;
            region.srcSubresource.baseArrayLayer = srcImage.arrayLayer;
            region.srcSubresource.layerCount = 1;
            region.srcOffset = {0, 0, 0};
            
            region.dstSubresource.aspectMask = planeAspect;
            region.dstSubresource.mipLevel = dstImage.mipLevel;
            region.dstSubresource.baseArrayLayer = dstImage.arrayLayer;
            region.dstSubresource.layerCount = 1;
            region.dstOffset = {0, 0, 0};
            
            // Calculate plane extent based on subsampling
            if (plane == 0) {
                region.extent = srcImage.extent;
            } else {
                uint32_t subsampledX = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledX;
                uint32_t subsampledY = 1 << mpInfo->planesLayout.secondaryPlaneSubsampledY;
                region.extent.width = (srcImage.extent.width + subsampledX - 1) / subsampledX;
                region.extent.height = (srcImage.extent.height + subsampledY - 1) / subsampledY;
                region.extent.depth = 1;
            }
        }
    }
}

VkResult VulkanFilterYuvCompute::RecordComputeDispatch(VkCommandBuffer cmdBuf,
                                                       uint32_t bufferIdx,
                                                       const FilterExecutionDesc& execDesc) {
    // Delegate to the image->image overload, which owns the descriptor binding and the
    // dispatch. Returning VK_SUCCESS here without recording anything would make
    // RecordCommandBuffer(execDesc) a transfers-only path that still reports
    // success: the caller gets its staging copies and an output image nothing
    // ever wrote. Refuse instead.
    if (execDesc.numInputs == 0 || execDesc.numOutputs == 0) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    const FilterIOSlot& inputSlot  = execDesc.inputs[0];
    const FilterIOSlot& outputSlot = execDesc.outputs[0];

    if (!inputSlot.primary.isValid() || !outputSlot.primary.isValid()) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    // The compute stage binds views, not raw images. Refuse rather than skip.
    if ((inputSlot.primaryView == nullptr) || (outputSlot.primaryView == nullptr)) {
        assert(!"FilterIOSlot::primaryView is required when the compute stage runs");
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    VkVideoPictureResourceInfoKHR inputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    inputResourceInfo.codedExtent = { inputSlot.primary.extent.width,
                                      inputSlot.primary.extent.height };
    inputResourceInfo.baseArrayLayer = execDesc.srcLayer;

    VkVideoPictureResourceInfoKHR outputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    outputResourceInfo.codedExtent = { outputSlot.primary.extent.width,
                                       outputSlot.primary.extent.height };
    outputResourceInfo.baseArrayLayer = execDesc.dstLayer;

    // A second output slot carries the subsampled-Y image when Adaptive Quantization is
    // enabled. It is optional and has no transfers of its own.
    const VkImageResourceView* subsampledView = nullptr;
    VkVideoPictureResourceInfoKHR subsampledResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    if ((execDesc.numOutputs > 1) && (execDesc.outputs[1].primaryView != nullptr)) {
        subsampledView = execDesc.outputs[1].primaryView;
        subsampledResourceInfo.codedExtent = { execDesc.outputs[1].primary.extent.width,
                                               execDesc.outputs[1].primary.extent.height };
        subsampledResourceInfo.baseArrayLayer = execDesc.dstLayer;
    }

    return RecordCommandBuffer(cmdBuf, bufferIdx,
                               inputSlot.primaryView,  &inputResourceInfo,
                               outputSlot.primaryView, &outputResourceInfo,
                               subsampledView,
                               subsampledView ? &subsampledResourceInfo : nullptr);
}

VkResult VulkanFilterYuvCompute::RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                                     uint32_t bufferIdx,
                                                     const FilterExecutionDesc& execDesc) {
    // ==========================================================================
    // Stage 1: Pre-Transfer Operations
    // ==========================================================================
    if (execDesc.hasAnyPreTransfer()) {
        RecordPreTransfers(cmdBuf, execDesc);
    }
    
    // ==========================================================================
    // Stage 2: Compute Operation (if not transfer-only mode)
    // ==========================================================================
    if (!m_skipCompute) {
        // Transition input images to GENERAL for compute read
        for (uint32_t i = 0; i < execDesc.numInputs; ++i) {
            TransferResource inputCopy = execDesc.inputs[i].primary;
            RecordLayoutTransition(cmdBuf, inputCopy, VK_IMAGE_LAYOUT_GENERAL,
                                  execDesc.hasAnyPreTransfer() ? 
                                      VK_PIPELINE_STAGE_TRANSFER_BIT : 
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  execDesc.hasAnyPreTransfer() ? 
                                      VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                                  VK_ACCESS_SHADER_READ_BIT);
        }
        
        // Transition output images to GENERAL for compute write
        for (uint32_t i = 0; i < execDesc.numOutputs; ++i) {
            TransferResource outputCopy = execDesc.outputs[i].primary;
            RecordLayoutTransition(cmdBuf, outputCopy, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, VK_ACCESS_SHADER_WRITE_BIT);
        }
        
        // Record compute dispatch
        VkResult result = RecordComputeDispatch(cmdBuf, bufferIdx, execDesc);
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    
    // ==========================================================================
    // Stage 3: Post-Transfer Operations
    // ==========================================================================
    if (execDesc.hasAnyPostTransfer()) {
        RecordPostTransfers(cmdBuf, execDesc);
    }
    
    return VK_SUCCESS;
}
