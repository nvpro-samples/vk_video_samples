/*
* Copyright 2020 NVIDIA Corporation.
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

#ifndef _VULKANVIDEOIMAGEPOOL_H_
#define _VULKANVIDEOIMAGEPOOL_H_

#include <assert.h>
#include <stdint.h>

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "vulkan_interfaces.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/VkImageResource.h"

class VulkanVideoImagePool;

class VulkanVideoImagePoolNode : public VkVideoRefCountBase {
public:

    // VulkanVideoImagePool is a friend class to be able to call SetParent()
    friend class VulkanVideoImagePool;

    VulkanVideoImagePoolNode()
        : m_vkDevCtx()
        , m_currentImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_stagedInputResidualLayout(VK_IMAGE_LAYOUT_MAX_ENUM)
        , m_pictureResourceInfo()
        , m_imageResourceView()
        , m_parent(nullptr)
        , m_parentIndex(-1)
        , m_recreateImage()
        , m_timelineSemaphore(VK_NULL_HANDLE)
        , m_semaphoreSubmitInfo()
    {
    }

    VkResult CreateImage( const VulkanDeviceContext* vkDevCtx,
                          const VkImageCreateInfo*   pImageCreateInfo,
                          VkMemoryPropertyFlags      requiredMemProps,
                          uint32_t                   imageIndex,
                          VkSharedBaseObj<VkImageResource>&  imageArrayParent,
                          VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                          VkImageAspectFlags aspectMask,
                          bool useLinear);

    /**
     * @brief Create a pool node from an externally-provided image resource view.
     *
     * This creates a non-owning pool node: the image and view are managed by the caller.
     * The pool node will NOT return to any pool when its ref-count drops to zero; it
     * simply releases the VkSharedBaseObj refs to the view (and transitively the image).
     *
     * Used by the external frame input path to wrap DMA-BUF-imported images as
     * VulkanVideoImagePoolNode so they can be set as srcStagingImageView or
     * srcEncodeImageResource in VkVideoEncodeFrameInfo.
     *
     * @param vkDevCtx          Vulkan device context
     * @param imageResourceView The image resource view to wrap (non-owning ref)
     * @param initialLayout     The current image layout
     * @param node              Output pool node
     * @return VK_SUCCESS on success
     */
    static VkResult CreateExternal(const VulkanDeviceContext* vkDevCtx,
                                   VkSharedBaseObj<VkImageResourceView>& imageResourceView,
                                   VkImageLayout initialLayout,
                                   VkSharedBaseObj<VulkanVideoImagePoolNode>& node);

    VkResult Init(const VulkanDeviceContext* vkDevCtx);

    void Deinit();

    VulkanVideoImagePoolNode (const VulkanVideoImagePoolNode &srcObj) = delete;
    VulkanVideoImagePoolNode (VulkanVideoImagePoolNode &&srcObj) = delete;

    ~VulkanVideoImagePoolNode()
    {
        Deinit();
    }

    bool GetImageView(VkSharedBaseObj<VkImageResourceView>& imageResourceView) {
        if (ImageExist()) {
            imageResourceView = m_imageResourceView;
            return true;
        }
        return false;
    }

    // Whether this node holds an image -- not whether that image has a view.
    // The two coincide for pool-allocated nodes, which always create image and
    // view together, but not for an external wrapper (CreateExternal): a
    // transfer-only staging import carries no view-compatible usage, so it has
    // an image and deliberately no view. Testing the view there would report a
    // node that plainly holds an image as empty.
    bool ImageExist() {

        return (!!m_imageResourceView && !!m_imageResourceView->GetImageResource() &&
                (m_imageResourceView->GetImageResource()->GetImage() != VK_NULL_HANDLE));
    }

    bool RecreateImage() {
        return !ImageExist() || m_recreateImage;
    }

    void RespecImage() {
        m_recreateImage = true;
    }

    bool SetNewLayout(VkImageLayout newImageLayout)
    {
        if (RecreateImage() || !ImageExist()) {
            return false;
        }

        m_currentImageLayout = newImageLayout;

        return true;
    }

    // === STAGED-INPUT RESIDUAL LAYOUT ===
    //
    // The layout VkVideoEncoder::StageInputFrame last LEFT this image in,
    // as a fact about work the library itself recorded -- not a
    // declaration.
    //
    // DELIBERATELY NOT m_currentImageLayout. That field is written by three
    // unrelated producers (CreateImage from a create-info, CreateExternal
    // from a caller-supplied declaration, and the pool handout in
    // GetImageSetNewLayout from a REQUESTED layout), so it answers "what did
    // somebody say" and not "what did we do". Reading it is what forced an
    // earlier attempt at this fix to be reverted, and the reason is that a
    // declaration cannot be a truth source about a barrier the library
    // recorded. This field has exactly ONE writer -- the handback at the end
    // of StageInputFrame, which sets it from the value that handback used as
    // its barrier newLayout -- and exactly ONE reader, the acquire at the top
    // of the NEXT StageInputFrame for the same registration.
    //
    // VK_IMAGE_LAYOUT_MAX_ENUM means "the library has not moved this image
    // yet", i.e. the caller's declaration is still the only fact available.
    // Every node starts there, including the per-frame wrapper the LEGACY
    // SubmitExternalFrame lane builds fresh on every call (VkVideoEncoder::
    // WrapExternalImage) -- which is why that lane keeps its pre-existing
    // behaviour byte for byte: a node that lives for one frame can never
    // carry a residual into a second one.
    bool HasStagedInputResidualLayout() const {
        return m_stagedInputResidualLayout != VK_IMAGE_LAYOUT_MAX_ENUM;
    }
    VkImageLayout GetStagedInputResidualLayout() const {
        return m_stagedInputResidualLayout;
    }
    void SetStagedInputResidualLayout(VkImageLayout residualLayout) {
        m_stagedInputResidualLayout = residualLayout;
    }

    VkVideoPictureResourceInfoKHR* GetPictureResourceInfo() { return &m_pictureResourceInfo; }

    int32_t GetImageIndex() { return m_parentIndex; }

    /**
     * @brief Get the timeline semaphore submit info for this image node
     * @return VkSemaphoreSubmitInfoKHR structure. If SetTimelineSemaphoreValue() was not called
     *         or failed, returns an empty structure with sType = VK_STRUCTURE_TYPE_APPLICATION_INFO (0),
     *         semaphore = VK_NULL_HANDLE, and value = 0.
     */
    VkSemaphoreSubmitInfoKHR GetSemaphoreSubmitInfo() const
    {
        return m_semaphoreSubmitInfo;
    }

    /**
     * @brief Set the timeline semaphore value for when the image processing completes
     * @param value The timeline semaphore value to signal when processing completes
     * @param stageMask The pipeline stage at which the image will be processed
     * @param deviceIndex The device index processing this image (default: 0)
     * @return VkSemaphoreSubmitInfoKHR structure with the semaphore info if successful.
     *         On error or when timeline semaphores are not supported, returns an empty structure
     *         with sType = VK_STRUCTURE_TYPE_APPLICATION_INFO (0), semaphore = VK_NULL_HANDLE,
     *         and value = 0.
     */
    VkSemaphoreSubmitInfoKHR SetTimelineSemaphoreValue(uint64_t value,
                                                       VkPipelineStageFlags2 stageMask,
                                                       uint32_t deviceIndex = 0);

private:
    VkResult SetParent(VkSharedBaseObj<VulkanVideoImagePool> imagePool, int32_t parentIndex);

private:
    const VulkanDeviceContext*            m_vkDevCtx;
    VkImageLayout                         m_currentImageLayout;
    // See the accessors above. MAX_ENUM == "the library has not moved it".
    VkImageLayout                         m_stagedInputResidualLayout;
    VkVideoPictureResourceInfoKHR         m_pictureResourceInfo;
    VkSharedBaseObj<VkImageResourceView>  m_imageResourceView;
    VkSharedBaseObj<VulkanVideoImagePool>  m_parent;
    int32_t                               m_parentIndex;
    uint32_t                              m_recreateImage : 1;
    VkSemaphore                           m_timelineSemaphore;
    VkSemaphoreSubmitInfoKHR              m_semaphoreSubmitInfo;
};

class VulkanVideoImagePool : public VkVideoRefCountBase,
                             public std::enable_shared_from_this<VulkanVideoImagePool> {
public:

    static constexpr size_t maxImages = 64;

    VulkanVideoImagePool()
        : m_vkDevCtx()
        , m_queueFamilyIndex((uint32_t)-1)
        , m_imageCreateInfo()
        , m_requiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_poolSize(0)
        , m_nextNodeToUse(0)
        , m_aspectMask(VK_IMAGE_ASPECT_COLOR_BIT)
        , m_usesImageArray(false)
        , m_usesImageViewArray(false)
        , m_usesLinearImage(false)
        , m_availablePoolNodes(0UL)
        , m_imageResources(maxImages)
        , m_imageArray()
        , m_imageViewArray()
    {
    }

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoImagePool>& imagePool);

    // For a pool whose images are written on one queue family and read on
    // another.
    //
    // Ownership of a VK_SHARING_MODE_EXCLUSIVE image does not move between
    // families by itself: without the release/acquire barrier pair, what the
    // reading family sees is undefined. Naming both families here creates the
    // images CONCURRENT instead. The list is copied -- the create info holds a
    // pointer into pool storage, not into the caller's -- and deduplicated, so
    // a session whose two families are the same still gets EXCLUSIVE, which is
    // what it should have.
    VkResult Configure(const VulkanDeviceContext*        vkDevCtx,
                       uint32_t                          numImages,
                       VkFormat                          imageFormat,
                       const VkExtent2D&                 maxImageExtent,
                       VkImageUsageFlags                 imageUsage,
                       const std::vector<uint32_t>&      queueFamilyIndices,
                       VkMemoryPropertyFlags             requiredMemProps,
                       const VkVideoProfileInfoKHR*      pVideoProfile,
                       VkImageAspectFlags                aspectMask,
                       bool                              useImageArray,
                       bool                              useImageViewArray,
                       bool                              useLinear,
                       uint64_t                          drmFormatModifier = 0,
                       VkImageLayout                     initialLayout =
                           VK_IMAGE_LAYOUT_UNDEFINED);

    VkResult Configure(const VulkanDeviceContext*   vkDevCtx,
                       uint32_t                     numImages,
                       VkFormat                     imageFormat,
                       const VkExtent2D&            maxImageExtent,
                       VkImageUsageFlags            imageUsage,
                       uint32_t                     queueFamilyIndex,
                       VkMemoryPropertyFlags        requiredMemProps,
                       const VkVideoProfileInfoKHR* pVideoProfile,
                       VkImageAspectFlags           aspectMask,
                       bool                         useImageArray,
                       bool                         useImageViewArray,
                       bool                         useLinear,
                       uint64_t                     drmFormatModifier = 0,
                       // The layout the images are CREATED in. UNDEFINED is
                       // right for every pool whose first writer is the
                       // device. A pool whose first writer is the HOST, through
                       // a persistent mapping of a LINEAR image, must say
                       // PREINITIALIZED instead: it is the only initial layout
                       // under which host writes performed before any barrier
                       // are preserved, and the only one a first acquire can
                       // legally name as its oldLayout.
                       VkImageLayout                initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    void Deinit();

    ~VulkanVideoImagePool()
    {
        Deinit();
    }

    VulkanVideoImagePoolNode& operator[](unsigned int index)
    {
        assert(index < m_imageResources.size());
        return m_imageResources[index];
    }

    size_t size()
    {
        return m_poolSize;
    }

    bool GetAvailableImage(VkSharedBaseObj<VulkanVideoImagePoolNode>&  imageResource,
                           VkImageLayout newImageLayout);

    bool ReleaseImageToPool(uint32_t imageIndex);

private:
    VkResult GetImageSetNewLayout(uint32_t imageIndex,
                                  VkImageLayout newImageLayout);

private:
    const VulkanDeviceContext*            m_vkDevCtx;
    std::mutex                            m_queueMutex;
    uint32_t                              m_queueFamilyIndex;
    // Stable storage for the create info's pQueueFamilyIndices when the pool
    // is shared across families. Deduplicated; a single entry means EXCLUSIVE.
    std::vector<uint32_t>                 m_queueFamilyIndices;
    VkVideoCoreProfile                    m_videoProfile;
    VkImageCreateInfo                     m_imageCreateInfo;
    VkMemoryPropertyFlags                 m_requiredMemProps;
    uint32_t                              m_poolSize;
    uint32_t                              m_nextNodeToUse;
    VkImageAspectFlags                    m_aspectMask;
    uint32_t                              m_usesImageArray : 1;
    uint32_t                              m_usesImageViewArray : 1;
    uint32_t                              m_usesLinearImage : 1;
    uint64_t                              m_availablePoolNodes;
    std::vector<VulkanVideoImagePoolNode> m_imageResources;
    VkSharedBaseObj<VkImageResource>      m_imageArray;     // must be valid if m_usesImageArray is true
    VkSharedBaseObj<VkImageResourceView>  m_imageViewArray; // must be valid if m_usesImageViewArray is true
};

#endif /* _VULKANVIDEOIMAGEPOOL_H_ */
