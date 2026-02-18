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

#include <atomic>
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VkImageResource.h"

VkImageResource::VkImageResource(const VulkanDeviceContext* vkDevCtx,
                const VkImageCreateInfo* pImageCreateInfo,
                VkImage image, VkDeviceSize imageOffset, VkDeviceSize imageSize,
                VkSharedBaseObj<VulkanDeviceMemoryImpl>& vulkanDeviceMemory,
                uint64_t drmFormatModifier,
                uint32_t memoryPlaneCount)
   : m_refCount(0), m_imageCreateInfo(*pImageCreateInfo), m_vkDevCtx(vkDevCtx)
   , m_image(image), m_imageOffset(imageOffset), m_imageSize(imageSize)
   , m_vulkanDeviceMemory(vulkanDeviceMemory), m_layouts{}, m_memoryPlaneLayouts{}
   , m_drmFormatModifier(drmFormatModifier), m_memoryPlaneCount(memoryPlaneCount)
   , m_isLinearImage(false), m_is16Bit(false), m_isSubsampledX(false), m_isSubsampledY(false)
   , m_usesDrmFormatModifier(drmFormatModifier != 0 || pImageCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
   , m_ownsResources(true) {

    // Query memory plane layouts for DRM format modifier images
    // Per Vulkan spec: For VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, must use
    // VK_IMAGE_ASPECT_MEMORY_PLANE_*_BIT_EXT aspect flags
    //
    // NVIDIA WORKAROUND: For non-disjoint multi-planar images with DRM modifiers,
    // the driver may return all zeros for MEMORY_PLANE_1 and higher because internally
    // it treats the image as having a single memory plane. In this case, we must
    // calculate the plane offsets manually based on the image format and dimensions.
    if (m_usesDrmFormatModifier && m_memoryPlaneCount > 0) {
        static const VkImageAspectFlagBits memoryPlaneAspects[] = {
            VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT
        };
        for (uint32_t p = 0; p < m_memoryPlaneCount && p < 4; ++p) {
            VkImageSubresource subRes{};
            subRes.aspectMask = memoryPlaneAspects[p];
            subRes.mipLevel = 0;
            subRes.arrayLayer = 0;
            m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subRes, &m_memoryPlaneLayouts[p]);
        }
        
        // NVIDIA WORKAROUND: If MEMORY_PLANE_1+ returned zeros, calculate offsets manually
        // This happens with non-disjoint multi-planar YCbCr images on NVIDIA
        const VkMpFormatInfo* ycbcrInfo = YcbcrVkFormatInfo(pImageCreateInfo->format);
        if (ycbcrInfo && m_memoryPlaneCount >= 2) {
            uint32_t width = pImageCreateInfo->extent.width;
            uint32_t height = pImageCreateInfo->extent.height;
            
            // Check if plane 1 layout is invalid (all zeros or size=0)
            if (m_memoryPlaneLayouts[1].size == 0 && m_memoryPlaneLayouts[1].rowPitch == 0) {
                // Calculate based on YCbCr format layout
                uint32_t bytesPerPixel = (ycbcrInfo->planesLayout.bpp == YCBCRA_8BPP) ? 1 : 2;
                
                // Plane 0 (Y): Full resolution
                if (m_memoryPlaneLayouts[0].rowPitch == 0) {
                    m_memoryPlaneLayouts[0].rowPitch = width * bytesPerPixel;
                }
                if (m_memoryPlaneLayouts[0].size == 0) {
                    m_memoryPlaneLayouts[0].size = m_memoryPlaneLayouts[0].rowPitch * height;
                }
                m_memoryPlaneLayouts[0].offset = 0;
                
                // Plane 1 (UV/CbCr): Subsampled based on format
                uint32_t chromaWidth = width;
                uint32_t chromaHeight = height;
                if (ycbcrInfo->planesLayout.secondaryPlaneSubsampledX) {
                    chromaWidth = (width + 1) / 2;
                }
                if (ycbcrInfo->planesLayout.secondaryPlaneSubsampledY) {
                    chromaHeight = (height + 1) / 2;
                }
                
                // For semi-planar (NV12, P010, etc.), UV plane has 2 components interleaved
                uint32_t uvBytesPerPixel = bytesPerPixel * 2;  // CbCr interleaved
                
                m_memoryPlaneLayouts[1].offset = m_memoryPlaneLayouts[0].size;
                m_memoryPlaneLayouts[1].rowPitch = chromaWidth * uvBytesPerPixel;
                m_memoryPlaneLayouts[1].size = m_memoryPlaneLayouts[1].rowPitch * chromaHeight;
                m_memoryPlaneLayouts[1].arrayPitch = 0;
                m_memoryPlaneLayouts[1].depthPitch = 0;
                
                // For 3-plane formats (I420, etc.)
                if (m_memoryPlaneCount >= 3 && 
                    m_memoryPlaneLayouts[2].size == 0 && m_memoryPlaneLayouts[2].rowPitch == 0) {
                    // Cb and Cr are separate planes, each single component
                    m_memoryPlaneLayouts[1].rowPitch = chromaWidth * bytesPerPixel;
                    m_memoryPlaneLayouts[1].size = m_memoryPlaneLayouts[1].rowPitch * chromaHeight;
                    
                    m_memoryPlaneLayouts[2].offset = m_memoryPlaneLayouts[1].offset + m_memoryPlaneLayouts[1].size;
                    m_memoryPlaneLayouts[2].rowPitch = chromaWidth * bytesPerPixel;
                    m_memoryPlaneLayouts[2].size = m_memoryPlaneLayouts[2].rowPitch * chromaHeight;
                    m_memoryPlaneLayouts[2].arrayPitch = 0;
                    m_memoryPlaneLayouts[2].depthPitch = 0;
                }
            }
        }
    }

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(pImageCreateInfo->format);
    VkImageSubresource subResource = {};
    if (mpInfo == nullptr) {
        m_isLinearImage = (pImageCreateInfo->tiling == VK_IMAGE_TILING_LINEAR);
        if (m_isLinearImage) {
            subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[0]);
        }
        return;
    }

    m_isSubsampledX = (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX);
    m_isSubsampledY = (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY);

    // Treat all non 8bpp formats as 16bpp for output to prevent any loss.
    m_is16Bit = (mpInfo->planesLayout.bpp != YCBCRA_8BPP);

    // External/non-owning wrapper (CreateFromExternal) has no VulkanDeviceMemoryImpl
    if (!vulkanDeviceMemory) {
        return;
    }
    VkMemoryPropertyFlags memoryPropertyFlags = vulkanDeviceMemory->GetMemoryPropertyFlags();
    if ((memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        return;
    }

    m_isLinearImage = true;
    bool isUnnormalizedRgba = false;
    if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
        isUnnormalizedRgba = true;
    }

    if (mpInfo && !isUnnormalizedRgba) {
        switch (mpInfo->planesLayout.layout) {
            case YCBCR_SINGLE_PLANE_UNNORMALIZED:
            case YCBCR_SINGLE_PLANE_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[0]);
                break;
            case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[1]);
                break;
            case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
            case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
            case YCBCR_PLANAR_STRIDE_PADDED:
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[0]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[1]);
                subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[2]);
                break;
            default:
                assert(0);
        }

    } else {
        m_vkDevCtx->GetImageSubresourceLayout(*vkDevCtx, image, &subResource, &m_layouts[0]);
    }
}

VkImageResource::~VkImageResource()
{
    Destroy();
}

VkResult VkImageResource::Create(const VulkanDeviceContext* vkDevCtx,
                                 const VkImageCreateInfo* pImageCreateInfo,
                                 VkMemoryPropertyFlags memoryPropertyFlags,
                                 VkSharedBaseObj<VkImageResource>& imageResource)
{
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;

    VkDevice device = vkDevCtx->getDevice();
    VkImage image = VK_NULL_HANDLE;

    do {

        result = vkDevCtx->CreateImage(device, pImageCreateInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            assert(!"CreateImage Failed!");
            break;
        }

        VkMemoryRequirements memoryRequirements = { };
        vkDevCtx->GetImageMemoryRequirements(device, image, &memoryRequirements);

        // Allocate memory for the image
        VkSharedBaseObj<VulkanDeviceMemoryImpl> vkDeviceMemory;
        result = VulkanDeviceMemoryImpl::Create(vkDevCtx,
                                                memoryRequirements,
                                                memoryPropertyFlags,
                                                nullptr,  // pInitializeMemory
                                                0ULL,     // initializeMemorySize
                                                false,    // clearMemory
                                                vkDeviceMemory);
        if (result != VK_SUCCESS) {
            assert(!"Create Memory Failed!");
            break;
        }

        VkDeviceSize imageOffset = 0;
        result = vkDevCtx->BindImageMemory(device, image, *vkDeviceMemory, imageOffset);
        if (result != VK_SUCCESS) {
            assert(!"BindImageMemory Failed!");
            break;
        }

        imageResource = new VkImageResource(vkDevCtx,
                                            pImageCreateInfo,
                                            image,
                                            imageOffset,
                                            memoryRequirements.size,
                                            vkDeviceMemory);
        if (imageResource == nullptr) {
            break;
        }
        return result;

    } while (0);

    if (device != VK_NULL_HANDLE) {

        if (image != VK_NULL_HANDLE) {
            vkDevCtx->DestroyImage(device, image, nullptr);
        }
    }

    return result;
}

VkResult VkImageResource::CreateExportable(const VulkanDeviceContext* vkDevCtx,
                                           const VkImageCreateInfo* pImageCreateInfo,
                                           VkMemoryPropertyFlags memoryPropertyFlags,
                                           VkExternalMemoryHandleTypeFlags exportHandleTypes,
                                           uint64_t drmFormatModifier,
                                           VkSharedBaseObj<VkImageResource>& imageResource)
{
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;

    VkDevice device = vkDevCtx->getDevice();
    VkImage image = VK_NULL_HANDLE;

    // Build modified image create info with external memory and DRM format modifier
    VkImageCreateInfo modifiedImageInfo = *pImageCreateInfo;

    // Setup DRM format modifier if specified
    VkImageDrmFormatModifierListCreateInfoEXT drmModList{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
    VkExternalMemoryImageCreateInfo extMemImageInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};

    // For MUTABLE_FORMAT_BIT with DRM modifiers, we need VkImageFormatListCreateInfo
    // Include the main format plus plane formats for YCbCr
    VkFormat viewFormats[4] = {};
    uint32_t viewFormatCount = 0;
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};

    // Check if DRM format modifier should be used:
    // - UINT64_MAX = sentinel for "no DRM modifier, use VK_IMAGE_TILING_OPTIMAL with opaque FD"
    // - DMA_BUF handle type requires DRM format modifier (modifier 0 = LINEAR is valid)
    // - Or explicit non-zero modifier was specified
    bool useDrmModifier = (drmFormatModifier != UINT64_MAX) &&
                          ((exportHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
                           (drmFormatModifier != 0));
    
    if (useDrmModifier || exportHandleTypes != 0) {
        extMemImageInfo.handleTypes = exportHandleTypes;

        if (useDrmModifier) {
            // Use DRM format modifier tiling (modifier 0 = DRM_FORMAT_MOD_LINEAR)
            modifiedImageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            drmModList.drmFormatModifierCount = 1;
            drmModList.pDrmFormatModifiers = &drmFormatModifier;

            // If MUTABLE_FORMAT_BIT is set, we need VkImageFormatListCreateInfo
            // per VUID-VkImageCreateInfo-tiling-02353
            if (pImageCreateInfo->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
                // Include the main format
                viewFormats[viewFormatCount++] = pImageCreateInfo->format;
                
                // For multi-planar YCbCr, also include plane formats from vkPlaneFormat[]
                const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(pImageCreateInfo->format);
                if (mpInfo && mpInfo->planesLayout.numberOfExtraPlanes > 0) {
                    // Add plane formats for NV12/NV21 (R8, RG8) or planar (R8, R8, R8) etc.
                    // Plane formats are in vkPlaneFormat[0..numberOfPlanes-1]
                    const uint32_t numPlanes = 1 + mpInfo->planesLayout.numberOfExtraPlanes;
                    for (uint32_t p = 0; p < numPlanes && p < VK_MAX_NUM_IMAGE_PLANES_EXT; ++p) {
                        if (mpInfo->vkPlaneFormat[p] != VK_FORMAT_UNDEFINED &&
                            mpInfo->vkPlaneFormat[p] != pImageCreateInfo->format) {
                            viewFormats[viewFormatCount++] = mpInfo->vkPlaneFormat[p];
                        }
                    }
                }
                
                formatList.viewFormatCount = viewFormatCount;
                formatList.pViewFormats = viewFormats;
                
                // Build pNext chain: formatList -> drmModList -> extMemImageInfo -> original pNext
                formatList.pNext = &drmModList;
                drmModList.pNext = &extMemImageInfo;
                extMemImageInfo.pNext = pImageCreateInfo->pNext;
                modifiedImageInfo.pNext = &formatList;
            } else {
                // No MUTABLE_FORMAT_BIT - no format list needed
                // Build pNext chain: drmModList -> extMemImageInfo -> original pNext
                drmModList.pNext = &extMemImageInfo;
                extMemImageInfo.pNext = pImageCreateInfo->pNext;
                modifiedImageInfo.pNext = &drmModList;
            }
        } else {
            // Just add external memory info
            extMemImageInfo.pNext = pImageCreateInfo->pNext;
            modifiedImageInfo.pNext = &extMemImageInfo;
        }
    }

    do {
        result = vkDevCtx->CreateImage(device, &modifiedImageInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            assert(!"CreateImage Failed!");
            break;
        }

        // Query the actual DRM format modifier used by the driver
        uint64_t actualDrmModifier = 0;
        uint32_t memoryPlaneCount = 1;  // Default for non-DRM images
        if (modifiedImageInfo.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageDrmFormatModifierPropertiesEXT modProps{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
            // Use PFN from device context (inherits from VkInterfaceFunctions)
            PFN_vkGetImageDrmFormatModifierPropertiesEXT pfnGetDrmMod =
                (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkDevCtx->GetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT");
            if (pfnGetDrmMod) {
                VkResult modResult = pfnGetDrmMod(device, image, &modProps);
                if (modResult == VK_SUCCESS) {
                    actualDrmModifier = modProps.drmFormatModifier;
                    // Warn if the driver returns DRM_FORMAT_MOD_INVALID — indicates a driver bug
                    if (actualDrmModifier == ((1ULL << 56) - 1)) {
                        std::cerr << "[VkImageResource] WARNING: vkGetImageDrmFormatModifierPropertiesEXT "
                                  << "returned DRM_FORMAT_MOD_INVALID — using requested modifier 0x"
                                  << std::hex << drmFormatModifier << std::dec << std::endl;
                        actualDrmModifier = drmFormatModifier;
                    }
                } else {
                    actualDrmModifier = drmFormatModifier;
                }
            } else {
                actualDrmModifier = drmFormatModifier; // Fallback to requested value
            }

            // Query plane count from format modifier properties
            // For now, assume same as color plane count (YCbCr formats typically pack in 1-3 memory planes)
            const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(pImageCreateInfo->format);
            if (mpInfo) {
                memoryPlaneCount = 1 + mpInfo->planesLayout.numberOfExtraPlanes;
            }
        }

        VkMemoryRequirements memoryRequirements = { };
        vkDevCtx->GetImageMemoryRequirements(device, image, &memoryRequirements);

        // Build export memory allocate info
        VkExportMemoryAllocateInfo exportMemInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportMemInfo.handleTypes = exportHandleTypes;

        // Query whether the export handle type requires dedicated allocation
        // (VUID-VkMemoryAllocateInfo-pNext-00639). DMA-BUF on NVIDIA typically
        // reports VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT.
        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = image;
        if (exportHandleTypes != 0) {
            VkPhysicalDeviceExternalImageFormatInfo extImageFormatInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
            extImageFormatInfo.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(exportHandleTypes);

            // Per VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02249:
            // when tiling is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, the
            // pNext chain must include VkPhysicalDeviceImageDrmFormatModifierInfoEXT
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT drmModInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
            drmModInfo.drmFormatModifier = actualDrmModifier;
            drmModInfo.sharingMode = modifiedImageInfo.sharingMode;

            // Per VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02313:
            // when tiling is DRM_FORMAT_MODIFIER_EXT and MUTABLE_FORMAT_BIT is
            // set, the chain must also include VkImageFormatListCreateInfo with
            // non-zero viewFormatCount. Build a separate copy for the query
            // (the image creation formatList has other structs chained in pNext).
            VkImageFormatListCreateInfo queryFormatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
            if (modifiedImageInfo.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
                if ((modifiedImageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
                    viewFormatCount > 0) {
                    queryFormatList.viewFormatCount = viewFormatCount;
                    queryFormatList.pViewFormats = viewFormats;
                    drmModInfo.pNext = &queryFormatList;
                }
                extImageFormatInfo.pNext = &drmModInfo;
            }

            VkPhysicalDeviceImageFormatInfo2 imageFormatQuery{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            imageFormatQuery.format = modifiedImageInfo.format;
            imageFormatQuery.type = modifiedImageInfo.imageType;
            imageFormatQuery.tiling = modifiedImageInfo.tiling;
            imageFormatQuery.usage = modifiedImageInfo.usage;
            imageFormatQuery.flags = modifiedImageInfo.flags;
            imageFormatQuery.pNext = &extImageFormatInfo;

            VkExternalImageFormatProperties extImageFormatProps{
                VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 imageFormatProps{
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            imageFormatProps.pNext = &extImageFormatProps;

            VkResult queryResult = vkDevCtx->GetPhysicalDeviceImageFormatProperties2(
                vkDevCtx->getPhysicalDevice(), &imageFormatQuery, &imageFormatProps);
            if (queryResult == VK_SUCCESS &&
                (extImageFormatProps.externalMemoryProperties.externalMemoryFeatures &
                 VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)) {
                exportMemInfo.pNext = &dedicatedInfo;
            }
        }

        // Allocate memory with export capabilities
        VkSharedBaseObj<VulkanDeviceMemoryImpl> vkDeviceMemory;
        result = VulkanDeviceMemoryImpl::CreateWithExport(vkDevCtx,
                                                          memoryRequirements,
                                                          memoryPropertyFlags,
                                                          (exportHandleTypes != 0) ? &exportMemInfo : nullptr,
                                                          nullptr,  // pInitializeMemory
                                                          0ULL,     // initializeMemorySize
                                                          false,    // clearMemory
                                                          vkDeviceMemory);
        if (result != VK_SUCCESS) {
            assert(!"Create Memory Failed!");
            break;
        }

        VkDeviceSize imageOffset = 0;
        result = vkDevCtx->BindImageMemory(device, image, *vkDeviceMemory, imageOffset);
        if (result != VK_SUCCESS) {
            assert(!"BindImageMemory Failed!");
            break;
        }

        imageResource = new VkImageResource(vkDevCtx,
                                            &modifiedImageInfo,
                                            image,
                                            imageOffset,
                                            memoryRequirements.size,
                                            vkDeviceMemory,
                                            actualDrmModifier,
                                            memoryPlaneCount);
        if (imageResource == nullptr) {
            break;
        }
        return result;

    } while (0);

    if (device != VK_NULL_HANDLE) {
        if (image != VK_NULL_HANDLE) {
            vkDevCtx->DestroyImage(device, image, nullptr);
        }
    }

    return result;
}

bool VkImageResource::IsExportable() const
{
    return m_vulkanDeviceMemory && m_vulkanDeviceMemory->IsExportable();
}

bool VkImageResource::GetMemoryPlaneLayout(uint32_t planeIndex, VkSubresourceLayout& layout) const
{
    if (!m_usesDrmFormatModifier || planeIndex >= m_memoryPlaneCount || planeIndex >= 4) {
        return false;
    }
    layout = m_memoryPlaneLayouts[planeIndex];
    return true;
}

#ifdef _WIN32
VkResult VkImageResource::ExportNativeHandle(VkExternalMemoryHandleTypeFlagBits handleType, void** outHandle) const
{
    if (!m_vulkanDeviceMemory) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return m_vulkanDeviceMemory->ExportNativeHandle(handleType, outHandle);
}
#else
VkResult VkImageResource::ExportNativeHandle(VkExternalMemoryHandleTypeFlagBits handleType, int* outHandle) const
{
    if (!m_vulkanDeviceMemory) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return m_vulkanDeviceMemory->ExportNativeHandle(handleType, outHandle);
}
#endif

uint32_t VkImageResource::GetMemoryTypeIndex() const
{
    if (!m_vulkanDeviceMemory) {
        return 0;
    }
    return m_vulkanDeviceMemory->GetMemoryTypeIndex();
}

VkResult VkImageResource::CreateFromExternal(const VulkanDeviceContext* vkDevCtx,
                                              VkImage image,
                                              VkDeviceMemory memory,
                                              const VkImageCreateInfo* pImageCreateInfo,
                                              VkSharedBaseObj<VkImageResource>& imageResource)
{
    // Create a non-owning wrapper: the caller owns the VkImage and VkDeviceMemory.
    // When this VkImageResource is destroyed, it will NOT destroy the VkImage or free the memory.

    VkSharedBaseObj<VulkanDeviceMemoryImpl> nullMemory; // no memory object (non-owning)

    VkImageResource* pImageResource = new VkImageResource(
        vkDevCtx, pImageCreateInfo,
        image,
        0,    // imageOffset
        0,    // imageSize (not known for external, not needed for non-owning)
        nullMemory,
        0,    // drmFormatModifier (not queried for external)
        0     // memoryPlaneCount (not queried for external)
    );

    if (pImageResource == nullptr) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Mark as non-owning so Destroy() won't call vkDestroyImage
    pImageResource->m_ownsResources = false;

    imageResource = pImageResource;
    return VK_SUCCESS;
}

VkResult VkImageResource::CreateFromImport(const VulkanDeviceContext* vkDevCtx,
                                            VkImage image,
                                            VkDeviceMemory memory,
                                            VkDeviceSize memorySize,
                                            const VkImageCreateInfo* pImageCreateInfo,
                                            VkSharedBaseObj<VkImageResource>& imageResource)
{
    // Wrap the imported memory in VulkanDeviceMemoryImpl so it gets freed
    // when the VkImageResource ref-count drops to zero.
    VkSharedBaseObj<VulkanDeviceMemoryImpl> deviceMemory;
    if (memory != VK_NULL_HANDLE) {
        deviceMemory = new VulkanDeviceMemoryImpl(vkDevCtx, memory, memorySize);
    }

    VkImageResource* pImageResource = new VkImageResource(
        vkDevCtx, pImageCreateInfo,
        image,
        0,           // imageOffset
        memorySize,
        deviceMemory,
        0,           // drmFormatModifier
        0            // memoryPlaneCount
    );

    if (pImageResource == nullptr) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Owning — Destroy() will call vkDestroyImage and the memory impl will vkFreeMemory
    pImageResource->m_ownsResources = true;

    imageResource = pImageResource;
    return VK_SUCCESS;
}

void VkImageResource::Destroy()
{
    assert(m_vkDevCtx != nullptr);

    if (m_ownsResources) {
        if (m_image != VK_NULL_HANDLE) {
            m_vkDevCtx->DestroyImage(*m_vkDevCtx, m_image, nullptr);
        }
    }
    // Always clear handles regardless of ownership
    m_image = VK_NULL_HANDLE;

    m_vulkanDeviceMemory = nullptr;
    m_vkDevCtx = nullptr;
}

// Overload without planeUsageOverride - calls the full version with 0
VkResult VkImageResourceView::Create(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<VkImageResource>& imageResource,
                                     VkImageSubresourceRange &imageSubresourceRange,
                                     VkSharedBaseObj<VkImageResourceView>& imageResourceView)
{
    return Create(vkDevCtx, imageResource, imageSubresourceRange, 0, imageResourceView);
}

// Full version with optional planeUsageOverride for storage-compatible plane views
VkResult VkImageResourceView::Create(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<VkImageResource>& imageResource,
                                     VkImageSubresourceRange &imageSubresourceRange,
                                     VkImageUsageFlags planeUsageOverride,
                                     VkSharedBaseObj<VkImageResourceView>& imageResourceView)
{
    VkDevice device = vkDevCtx->getDevice();
    VkImageView  imageViews[4];
    uint32_t numViews = 0;
    uint32_t numPlanes = 0;
    VkImageViewCreateInfo viewInfo = VkImageViewCreateInfo();
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = nullptr;
    viewInfo.image = imageResource->GetImage();
    viewInfo.viewType = (imageSubresourceRange.layerCount > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageResource->GetImageCreateInfo().format;
    viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = imageSubresourceRange;
    viewInfo.flags = 0;

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(viewInfo.format);
    const VkImageCreateInfo& imageCreateInfo = imageResource->GetImageCreateInfo();

    // For multi-planar formats with planeUsageOverride, skip the combined view (index 0)
    // as the combined format may not support the requested usage (e.g., STORAGE)
    bool skipCombinedView = (mpInfo != nullptr) && (planeUsageOverride != 0);

    // Setup VkImageViewUsageCreateInfo - required when using EXTENDED_USAGE_BIT per Khronos issue #4624:
    // Combined views must restrict usage to what the multi-planar format supports (exclude STORAGE)
    // Per-plane views must restrict usage to what the plane format supports (exclude VIDEO_ENCODE_SRC)
    VkImageViewUsageCreateInfo usageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usageCreateInfo.pNext = nullptr;
    usageCreateInfo.usage = planeUsageOverride;

    if (!skipCombinedView) {
        // For multi-planar formats, combined views cannot have STORAGE_BIT
        // (multi-planar formats don't support storage - only per-plane views do).
        // Must use VkImageViewUsageCreateInfo to restrict usage when the image
        // has STORAGE_BIT via EXTENDED_USAGE_BIT.
        //
        // Note: SAMPLED_BIT must NOT be stripped here. The decoder's display
        // pipeline uses the combined view with VkSamplerYcbcrConversion for
        // sampling decoded frames. Stripping SAMPLED_BIT causes blank output.
        // The VUID-VkImageViewCreateInfo-format-06415 validation error for
        // SAMPLED_BIT without YCbCr conversion is handled by the caller
        // creating a YCbCr sampler for the combined view before sampling.
        //
        // Reference: https://gitlab.khronos.org/vulkan/vulkan/-/issues/4624
        if (mpInfo && (imageCreateInfo.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            VkImageUsageFlags combinedUsage = imageCreateInfo.usage;
            // Remove STORAGE - not supported by multi-planar base format
            combinedUsage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
            usageCreateInfo.usage = combinedUsage;
            viewInfo.pNext = &usageCreateInfo;
        }
        
        VkResult result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
        if (result != VK_SUCCESS) {
            return result;
        }
        numViews++;
        
        // Reset pNext for subsequent views
        viewInfo.pNext = nullptr;
    } else {
        // Set placeholder for combined view
        imageViews[numViews] = VK_NULL_HANDLE;
        numViews++;
    }

    // Reset usage for plane views
    usageCreateInfo.usage = planeUsageOverride;

    if (mpInfo) { // Is this a YCbCr format

        // Create separate image views for Y and CbCr planes
        viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];  // For the Y plane
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;

        // Chain usage override if specified
        if (planeUsageOverride != 0) {
            viewInfo.pNext = &usageCreateInfo;
        }

        VkResult result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
        if (result != VK_SUCCESS) {
            return result;
        }
        numViews++;
        numPlanes++;

        if (mpInfo->planesLayout.numberOfExtraPlanes > 0) {
            viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];  // For the CbCr plane
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;
            result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
            if (result != VK_SUCCESS) {
                return result;
            }
            numViews++;
            numPlanes++;

            if (mpInfo->planesLayout.numberOfExtraPlanes > 1) {
                viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];  // For the CbCr plane
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;
                result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
                if (result != VK_SUCCESS) {
                    return result;
                }
                numViews++;
                numPlanes++;
            }
        }

        // Reset pNext after plane views are created
        viewInfo.pNext = nullptr;
    } else {

        // Is this a plane of YCbCr format

        const VkImageAspectFlags yCbCrAspectMask =
                imageSubresourceRange.aspectMask & (VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                    VK_IMAGE_ASPECT_PLANE_1_BIT |
                                                    VK_IMAGE_ASPECT_PLANE_2_BIT);

        while (yCbCrAspectMask != 0) {

            // possible Single Plane
            bool possibleSinglePlane = false;

            // Handle the Y only and CbCr only images
            switch(viewInfo.format) {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R16_UNORM:
            case VK_FORMAT_R10X6_UNORM_PACK16:
            case VK_FORMAT_R12X4_UNORM_PACK16:
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R16G16_UNORM:
            case VK_FORMAT_R32_UINT:
            case VK_FORMAT_R8_SINT:
            case VK_FORMAT_R8G8_SINT:
                possibleSinglePlane = true;
                break;
            default:
                break;
            }

            if (!possibleSinglePlane) {
                break;
            }

            for (uint32_t planeNum = 0; planeNum < 3; planeNum++) {
                if (yCbCrAspectMask & (VK_IMAGE_ASPECT_PLANE_0_BIT << planeNum)) {
                    numPlanes++;
                }
            }

            // Is this a single plane?
            if (numPlanes > 1) {
                // It is not a single plane.
                // Reset the plane count to 0.
                numPlanes = 0;
            }

            break;
        }
        
        // For regular single-plane formats (RGBA, etc.) that didn't match the YCbCr plane
        // handling above, set numPlanes = 1 since there's effectively one color plane
        if (numPlanes == 0) {
            numPlanes = 1;
        }
    }

    imageResourceView = new VkImageResourceView(vkDevCtx, imageResource,
                                                numViews, numPlanes,
                                                imageViews, imageSubresourceRange);

    return VK_SUCCESS;
}

// Full version with YCbCr sampler conversion support
// Creates both storage-compatible plane views AND a sampled combined view with YCbCr conversion
VkResult VkImageResourceView::Create(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<VkImageResource>& imageResource,
                                     VkImageSubresourceRange &imageSubresourceRange,
                                     VkImageUsageFlags planeUsageOverride,
                                     VkSamplerYcbcrConversion ycbcrConversion,
                                     VkImageUsageFlags combinedViewUsage,
                                     VkSharedBaseObj<VkImageResourceView>& imageResourceView)
{
    VkDevice device = vkDevCtx->getDevice();
    VkImageView  imageViews[4] = {VK_NULL_HANDLE};
    uint32_t numViews = 0;
    uint32_t numPlanes = 0;
    
    VkImageViewCreateInfo viewInfo = VkImageViewCreateInfo();
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = nullptr;
    viewInfo.image = imageResource->GetImage();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;  // Combined view is always 2D for display
    viewInfo.format = imageResource->GetImageCreateInfo().format;
    viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = imageSubresourceRange;
    viewInfo.subresourceRange.layerCount = 1;  // Combined view uses single layer
    viewInfo.flags = 0;

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(viewInfo.format);

    // Create combined view with YCbCr conversion for display sampling
    // Chain: viewInfo -> ycbcrConversionInfo -> usageCreateInfo
    VkSamplerYcbcrConversionInfo ycbcrConversionInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    ycbcrConversionInfo.conversion = ycbcrConversion;
    ycbcrConversionInfo.pNext = nullptr;
    
    VkImageViewUsageCreateInfo combinedUsageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    combinedUsageInfo.usage = combinedViewUsage;
    combinedUsageInfo.pNext = nullptr;
    
    // Build pNext chain for combined view
    if (ycbcrConversion != VK_NULL_HANDLE) {
        if (combinedViewUsage != 0) {
            ycbcrConversionInfo.pNext = &combinedUsageInfo;
        }
        viewInfo.pNext = &ycbcrConversionInfo;
    } else if (combinedViewUsage != 0) {
        viewInfo.pNext = &combinedUsageInfo;
    }
    
    // Create combined view (index 0)
    VkResult result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
    if (result != VK_SUCCESS) {
        return result;
    }
    numViews++;
    
    // Now create per-plane views for compute storage
    if (mpInfo) {
        // Reset pNext for plane views (use planeUsageOverride instead)
        viewInfo.pNext = nullptr;
        viewInfo.viewType = (imageSubresourceRange.layerCount > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange = imageSubresourceRange;
        
        VkImageViewUsageCreateInfo planeUsageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        planeUsageInfo.usage = planeUsageOverride;
        
        if (planeUsageOverride != 0) {
            viewInfo.pNext = &planeUsageInfo;
        }
        
        // Create Y plane view
        viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;
        result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
        if (result != VK_SUCCESS) {
            // Clean up combined view on failure
            vkDevCtx->DestroyImageView(device, imageViews[0], nullptr);
            return result;
        }
        numViews++;
        numPlanes++;

        // Create additional plane views
        if (mpInfo->planesLayout.numberOfExtraPlanes > 0) {
            viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;
            result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
            if (result != VK_SUCCESS) {
                // Clean up on failure
                for (uint32_t i = 0; i < numViews; i++) {
                    if (imageViews[i]) vkDevCtx->DestroyImageView(device, imageViews[i], nullptr);
                }
                return result;
            }
            numViews++;
            numPlanes++;

            if (mpInfo->planesLayout.numberOfExtraPlanes > 1) {
                viewInfo.format = mpInfo->vkPlaneFormat[numPlanes];
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << numPlanes;
                result = vkDevCtx->CreateImageView(device, &viewInfo, nullptr, &imageViews[numViews]);
                if (result != VK_SUCCESS) {
                    // Clean up on failure
                    for (uint32_t i = 0; i < numViews; i++) {
                        if (imageViews[i]) vkDevCtx->DestroyImageView(device, imageViews[i], nullptr);
                    }
                    return result;
                }
                numViews++;
                numPlanes++;
            }
        }
    }

    imageResourceView = new VkImageResourceView(vkDevCtx, imageResource,
                                                numViews, numPlanes,
                                                imageViews, imageSubresourceRange);

    return VK_SUCCESS;
}

VkImageResourceView::~VkImageResourceView()
{
    for (uint32_t imageViewIndx = 0; imageViewIndx < m_numViews; imageViewIndx++) {
        if (m_imageViews[imageViewIndx] != VK_NULL_HANDLE) {
            m_vkDevCtx->DestroyImageView(*m_vkDevCtx, m_imageViews[imageViewIndx], nullptr);
            m_imageViews[imageViewIndx] = VK_NULL_HANDLE;
        }
    }

    m_imageResource = nullptr;
    m_vkDevCtx = nullptr;
}
