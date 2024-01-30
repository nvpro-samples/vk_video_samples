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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <vector>
#include <queue>
#include <sstream>
#include <string.h>
#include <string>

#include "vulkan_interfaces.h"
#include "vkvideo_parser/PictureBufferBase.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VulkanVideoEncodeFrameBuffer.h"
#include "VkCodecUtils/VkImageResource.h"

static VkSharedBaseObj<VkImageResourceView> emptyImageView;

class NvPerFrameEncodeResources : public vkPicBuffBase {
public:
    NvPerFrameEncodeResources()
        : m_picDispInfo()
        , m_frameCompleteFence()
        , m_frameCompleteSemaphore()
        , m_frameConsumerDoneFence()
        , m_frameConsumerDoneSemaphore()
        , m_hasFrameCompleteSignalFence(false)
        , m_hasFrameCompleteSignalSemaphore(false)
        , m_hasConsummerSignalFence(false)
        , m_hasConsummerSignalSemaphore(false)
        , m_inEncodeQueue(false)
        , m_inDisplayQueue(false)
        , m_ownedByDisplay(false)
        , m_recreateImage(false)
        , m_currentDpbImageLayerLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_currentInputImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_vkDevCtx()
        , m_frameDpbImageView()
        , m_inImageView()
    {
    }

    VkResult CreateImage( const VulkanDeviceContext* vkDevCtx,
                          const VkImageCreateInfo* pDpbImageCreateInfo,
                          const VkImageCreateInfo* pInImageCreateInfo,
                          VkMemoryPropertyFlags    dpbRequiredMemProps,
                          VkMemoryPropertyFlags    inRequiredMemProps,
                          uint32_t imageIndex,
                          VkSharedBaseObj<VkImageResource>&  imageArrayParent,
                          VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                          bool useSeparateInputImage = false,
                          bool useLinearInput = false);

    VkResult init( const VulkanDeviceContext* vkDevCtx);

    void Deinit();

    NvPerFrameEncodeResources (const NvPerFrameEncodeResources &srcObj) = delete;
    NvPerFrameEncodeResources (NvPerFrameEncodeResources &&srcObj) = delete;

    ~NvPerFrameEncodeResources()
    {
        Deinit();
    }

    VkSharedBaseObj<VkImageResourceView>& GetFrameImageView() {
        if (ImageExist()) {
            return m_frameDpbImageView;
        } else {
            return emptyImageView;
        }
    }

    VkSharedBaseObj<VkImageResourceView>& GetDisplayImageView() {
        if (ImageExist()) {
            return m_inImageView;
        } else {
            return emptyImageView;
        }
    }

    bool ImageExist() {

        return (!!m_frameDpbImageView && (m_frameDpbImageView->GetImageView() != VK_NULL_HANDLE));
    }

    bool GetImageSetNewLayout(VkImageLayout newDpbImageLayout,
                              VkVideoPictureResourceInfoKHR* pDpbPictureResource,
                              VulkanVideoEncodeFrameBuffer::PictureResourceInfo* pDpbPictureResourceInfo,
                              VkImageLayout newInputImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM,
                              VkVideoPictureResourceInfoKHR* pInputPictureResource = nullptr,
                              VulkanVideoEncodeFrameBuffer::PictureResourceInfo* pInputPictureResourceInfo = nullptr) {


        if (m_recreateImage || !ImageExist()) {
            return false;
        }

        if (pDpbPictureResourceInfo) {
            pDpbPictureResourceInfo->image = m_frameDpbImageView->GetImageResource()->GetImage();
            pDpbPictureResourceInfo->imageFormat = m_frameDpbImageView->GetImageResource()->GetImageCreateInfo().format;
            pDpbPictureResourceInfo->currentImageLayout = m_currentDpbImageLayerLayout;
        }

        if (VK_IMAGE_LAYOUT_MAX_ENUM != newDpbImageLayout) {
            m_currentDpbImageLayerLayout = newDpbImageLayout;
        }

        if (pDpbPictureResource) {
            pDpbPictureResource->imageViewBinding = m_frameDpbImageView->GetImageView();
        }

        if (pInputPictureResourceInfo) {
            pInputPictureResourceInfo->image = m_inImageView->GetImageResource()->GetImage();
            pInputPictureResourceInfo->imageFormat = m_inImageView->GetImageResource()->GetImageCreateInfo().format;
            pInputPictureResourceInfo->currentImageLayout = m_currentInputImageLayout;
        }

        if (VK_IMAGE_LAYOUT_MAX_ENUM != newInputImageLayout) {
            m_currentInputImageLayout = newInputImageLayout;
        }

        if (pInputPictureResource) {
            pInputPictureResource->imageViewBinding = m_inImageView->GetImageView();
        }

        return true;
    }

    VkEncodePictureInfo m_picDispInfo;
    VkFence m_frameCompleteFence;
    VkSemaphore m_frameCompleteSemaphore;
    VkFence m_frameConsumerDoneFence;
    VkSemaphore m_frameConsumerDoneSemaphore;
    uint32_t m_hasFrameCompleteSignalFence : 1;
    uint32_t m_hasFrameCompleteSignalSemaphore : 1;
    uint32_t m_hasConsummerSignalFence : 1;
    uint32_t m_hasConsummerSignalSemaphore : 1;
    uint32_t m_inEncodeQueue : 1;
    uint32_t m_inDisplayQueue : 1;
    uint32_t m_ownedByDisplay : 1;
    uint32_t m_recreateImage : 1;
    // VPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdVps;
    // SPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdSps;
    // PPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdPps;
    // The bitstream Buffer
    VkSharedBaseObj<VkVideoRefCountBase>  bitstreamData;

private:
    VkImageLayout                        m_currentDpbImageLayerLayout;
    VkImageLayout                        m_currentInputImageLayout;
    const VulkanDeviceContext*           m_vkDevCtx;
    VkSharedBaseObj<VkImageResourceView> m_frameDpbImageView;
    VkSharedBaseObj<VkImageResourceView> m_inImageView;
};

class NvPerFrameEncodeImageSet {
public:

    static constexpr size_t maxImages = 32;

    NvPerFrameEncodeImageSet()
        : m_queueFamilyIndex((uint32_t)-1)
        , m_dpbImageCreateInfo()
        , m_inImageCreateInfo()
        , m_dpbRequiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_inRequiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_numImages(0)
        , m_usesImageArray(false)
        , m_usesImageViewArray(false)
        , m_usesSeparateInputImage(false)
        , m_usesLinearInput(false)
        , m_perFrameEncodeResources(maxImages)
        , m_imageArray()
        , m_imageViewArray()
    {
    }

    int32_t init(const VulkanDeviceContext* vkDevCtx,
        const VkVideoProfileInfoKHR* pEncodeProfile,
        uint32_t              numImages,
        VkFormat              dpbImageFormat,
        VkFormat              inImageFormat,
        const VkExtent2D&     maxImageExtent,
        VkImageUsageFlags     dpbImageUsage,
        VkImageUsageFlags     inImageUsage,
        uint32_t              queueFamilyIndex,
        VkMemoryPropertyFlags dpbRequiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VkMemoryPropertyFlags inRequiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        bool useImageArray = false,
        bool useImageViewArray = false,
        bool useSeparateInputImages = false,
        bool useLinearInput = false);

    void Deinit();

    ~NvPerFrameEncodeImageSet()
    {
        Deinit();
    }

    NvPerFrameEncodeResources& operator[](unsigned int index)
    {
        assert(index < m_perFrameEncodeResources.size());
        return m_perFrameEncodeResources[index];
    }

    size_t size()
    {
        return m_numImages;
    }

    VkResult GetImageSetNewLayout(const VulkanDeviceContext* vkDevCtx,
                                  uint32_t imageIndex,
                                  VkImageLayout newDpbImageLayout,
                                  VkVideoPictureResourceInfoKHR* pDpbPictureResource = nullptr,
                                  VulkanVideoEncodeFrameBuffer::PictureResourceInfo* pDpbPictureResourceInfo = nullptr,
                                  VkImageLayout newInputImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM,
                                  VkVideoPictureResourceInfoKHR* pInputPictureResource = nullptr,
                                  VulkanVideoEncodeFrameBuffer::PictureResourceInfo* pInputPictureResourceInfo = nullptr) {

        VkResult result = VK_SUCCESS;
        if (pDpbPictureResource) {
            if (m_imageViewArray) {
                // We have an image view that has the same number of layers as the image.
                // In that scenario, while specifying the resource, the API must specifically choose the image layer.
                pDpbPictureResource->baseArrayLayer = imageIndex;
            } else {
                // Let the image view sub-resource specify the image layer.
                pDpbPictureResource->baseArrayLayer = 0;
            }
        }

        if(pInputPictureResource) {
            // Input pictures currently are only allocated as discrete
            // Let the image view sub-resource specify the image layer.
            pInputPictureResource->baseArrayLayer = 0;
        }

        bool validImage = m_perFrameEncodeResources[imageIndex].GetImageSetNewLayout(
                               newDpbImageLayout,
                               pDpbPictureResource,
                               pDpbPictureResourceInfo,
                               newInputImageLayout,
                               pInputPictureResource,
                               pInputPictureResourceInfo);

        if (!validImage) {
            result = m_perFrameEncodeResources[imageIndex].CreateImage(
                               vkDevCtx,
                               &m_dpbImageCreateInfo,
                               &m_inImageCreateInfo,
                               m_dpbRequiredMemProps,
                               m_inRequiredMemProps,
                               imageIndex,
                               m_imageArray,
                               m_imageViewArray,
                               m_usesSeparateInputImage,
                               m_usesLinearInput);

            if (result == VK_SUCCESS) {
                validImage = m_perFrameEncodeResources[imageIndex].GetImageSetNewLayout(
                                               newDpbImageLayout,
                                               pDpbPictureResource,
                                               pDpbPictureResourceInfo,
                                               newInputImageLayout,
                                               pInputPictureResource,
                                               pInputPictureResourceInfo);

                assert(validImage);
            }
        }

        return result;
    }

private:
    uint32_t                             m_queueFamilyIndex;
    VkVideoCoreProfile                   m_videoProfile;
    VkImageCreateInfo                    m_dpbImageCreateInfo;
    VkImageCreateInfo                    m_inImageCreateInfo;
    VkMemoryPropertyFlags                m_dpbRequiredMemProps;
    VkMemoryPropertyFlags                m_inRequiredMemProps;
    uint32_t                             m_numImages;
    uint32_t                             m_usesImageArray:1;
    uint32_t                             m_usesImageViewArray:1;
    uint32_t                             m_usesSeparateInputImage:1;
    uint32_t                             m_usesLinearInput:1;
    std::vector<NvPerFrameEncodeResources> m_perFrameEncodeResources;
    VkSharedBaseObj<VkImageResource>     m_imageArray;     // must be valid if m_usesImageArray is true
    VkSharedBaseObj<VkImageResourceView> m_imageViewArray; // must be valid if m_usesImageViewArray is true
};

class VkVideoEncodeFrameBuffer : public VulkanVideoEncodeFrameBuffer {
public:

    static constexpr size_t maxFramebufferImages = 32;

    VkVideoEncodeFrameBuffer(const VulkanDeviceContext* vkDevCtx)
        : m_vkDevCtx(vkDevCtx)
        , m_refCount(0)
        , m_displayQueueMutex()
        , m_perFrameEncodeImageSet()
        , m_displayFrames()
        , m_queryPool()
        , m_ownedByDisplayMask(0)
        , m_frameNumInDisplayOrder(0)
        , m_codedExtent { 0, 0 }
        , m_numberParameterUpdates(0)
        , m_debug()
    {
    }

    virtual int32_t AddRef();
    virtual int32_t Release();

    VkResult CreateVideoQueries(uint32_t numSlots, const VulkanDeviceContext* vkDevCtx, const VkVideoProfileInfoKHR* pEncodeProfile)
    {
        assert (numSlots <= maxFramebufferImages);

        if ((m_queryPool == VK_NULL_HANDLE) && m_vkDevCtx->GetVideoEncodeQueryResultStatusSupport()) {
            // It would be difficult to resize a query pool, so allocate the maximum possible slot.
            numSlots = maxFramebufferImages;
            VkQueryPoolCreateInfo queryPoolCreateInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryPoolCreateInfo.pNext = pEncodeProfile;
            queryPoolCreateInfo.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
            queryPoolCreateInfo.queryCount = numSlots; // m_numEncodeSurfaces frames worth

            return m_vkDevCtx->CreateQueryPool(*vkDevCtx, &queryPoolCreateInfo, NULL, &m_queryPool);
        }

        return VK_SUCCESS;
    }

    void DestroyVideoQueries() {
        if (m_queryPool != VkQueryPool()) {
            m_vkDevCtx->DestroyQueryPool(*m_vkDevCtx, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }
    }

    uint32_t  FlushDisplayQueue()
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        uint32_t flushedImages = 0;
        while (!m_displayFrames.empty()) {
            int8_t pictureIndex = m_displayFrames.front();
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameEncodeImageSet.size()));
            m_displayFrames.pop();
            if (m_perFrameEncodeImageSet[(uint32_t)pictureIndex].IsAvailable()) {
                // The frame is not released yet - force release it.
                m_perFrameEncodeImageSet[(uint32_t)pictureIndex].Release();
            }
            flushedImages++;
        }

        return flushedImages;
    }

    virtual int32_t InitImagePool(const VkVideoProfileInfoKHR* pEncodeProfile,
                                  uint32_t                 numImages,
                                  VkFormat                 dpbImageFormat,
                                  VkFormat                 inImageFormat,
                                  const VkExtent2D&        codedExtent,
                                  const VkExtent2D&        maxImageExtent,
                                  VkImageUsageFlags        dpbImageUsage,
                                  VkImageUsageFlags        inImageUsage,
                                  uint32_t                 queueFamilyIndex,
                                  int32_t                  numImagesToPreallocate,
                                  bool                     useImageArray = false,
                                  bool                     useImageViewArray = false,
                                  bool                     useSeparateInputImage = false,
                                  bool                     useLinearInput = false)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        assert(numImages && (numImages <= maxFramebufferImages) && pEncodeProfile);

        VkResult result = CreateVideoQueries(numImages, m_vkDevCtx, pEncodeProfile);
        if (result != VK_SUCCESS) {
            return 0;
        }

        // m_extent is for the codedExtent, not the max image resolution
        m_codedExtent = codedExtent;

        int32_t imageSetCreateResult =
                m_perFrameEncodeImageSet.init(m_vkDevCtx,
                                              pEncodeProfile,
                                              numImages,
                                              dpbImageFormat,
                                              inImageFormat,
                                              maxImageExtent,
                                              dpbImageUsage,
                                              inImageUsage,
                                              queueFamilyIndex,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              useLinearInput ? ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT)  :
                                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              useImageArray, useImageViewArray,
                                              useSeparateInputImage, useLinearInput);
        m_numberParameterUpdates++;

        return imageSetCreateResult;
    }

    void Deinitialize() {

        FlushDisplayQueue();

        DestroyVideoQueries();

        m_ownedByDisplayMask = 0;
        m_frameNumInDisplayOrder = 0;

        m_perFrameEncodeImageSet.Deinit();

        if (m_queryPool != VkQueryPool()) {
            m_vkDevCtx->DestroyQueryPool(*m_vkDevCtx, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }
    };

    virtual int32_t QueueEncodedPictureForDisplay(int8_t picId, VulkanVideoDisplayPictureInfo* pDispInfo)
    {
        assert((uint32_t)picId < m_perFrameEncodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameEncodeImageSet[picId].m_displayOrder = m_frameNumInDisplayOrder++;
        m_perFrameEncodeImageSet[picId].m_timestamp = pDispInfo->timestamp;
        m_perFrameEncodeImageSet[picId].m_inDisplayQueue = true;
        m_perFrameEncodeImageSet[picId].AddRef();

        m_displayFrames.push((uint8_t)picId);

        if (m_debug) {
            std::cout << "==> Queue Display Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameEncodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameEncodeImageSet[picId].m_decodeOrder
                      << "\ttimestamp " << m_perFrameEncodeImageSet[picId].m_timestamp << std::endl;
        }
        return picId;
    }

    virtual int32_t QueuePictureForEncode(int8_t picId, VkEncodePictureInfo* pEncodePictureInfo,
                                          ReferencedObjectsInfo* pReferencedObjectsInfo,
                                          FrameSynchronizationInfo* pFrameSynchronizationInfo)
    {
        assert((uint32_t)picId < m_perFrameEncodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameEncodeImageSet[picId].m_picDispInfo = *pEncodePictureInfo;
        m_perFrameEncodeImageSet[picId].m_inEncodeQueue = true;
        m_perFrameEncodeImageSet[picId].stdPps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdPps);
        m_perFrameEncodeImageSet[picId].stdSps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdSps);
        m_perFrameEncodeImageSet[picId].stdVps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdVps);
        m_perFrameEncodeImageSet[picId].bitstreamData = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pBitstreamData);

        if (m_debug) {
            std::cout << "==> Queue Encode Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameEncodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameEncodeImageSet[picId].m_decodeOrder
                      << "\tFrameType " << m_perFrameEncodeImageSet[picId].m_picDispInfo.videoFrameType << std::endl;
        }

        if (pFrameSynchronizationInfo->hasFrameCompleteSignalFence) {
            pFrameSynchronizationInfo->frameCompleteFence = m_perFrameEncodeImageSet[picId].m_frameCompleteFence;
            if (pFrameSynchronizationInfo->frameCompleteFence) {
                m_perFrameEncodeImageSet[picId].m_hasFrameCompleteSignalFence = true;
            }
        }

        if (m_perFrameEncodeImageSet[picId].m_hasConsummerSignalFence) {
            pFrameSynchronizationInfo->frameConsumerDoneFence = m_perFrameEncodeImageSet[picId].m_frameConsumerDoneFence;
            m_perFrameEncodeImageSet[picId].m_hasConsummerSignalFence = false;
        }

        if (pFrameSynchronizationInfo->hasFrameCompleteSignalSemaphore) {
            pFrameSynchronizationInfo->frameCompleteSemaphore = m_perFrameEncodeImageSet[picId].m_frameCompleteSemaphore;
            if (pFrameSynchronizationInfo->frameCompleteSemaphore) {
                m_perFrameEncodeImageSet[picId].m_hasFrameCompleteSignalSemaphore = true;
            }
        }

        if (m_perFrameEncodeImageSet[picId].m_hasConsummerSignalSemaphore) {
            pFrameSynchronizationInfo->frameConsumerDoneSemaphore = m_perFrameEncodeImageSet[picId].m_frameConsumerDoneSemaphore;
            m_perFrameEncodeImageSet[picId].m_hasConsummerSignalSemaphore = false;
        }

        pFrameSynchronizationInfo->queryPool = m_queryPool;
        pFrameSynchronizationInfo->startQueryId = picId;
        pFrameSynchronizationInfo->numQueries = 1;

        return picId;
    }

    // dequeue
    virtual int32_t DequeueEncodedPicture(EncodingFrame* pEncodedFrame)
    {
        int numberofPendingFrames = 0;
        int pictureIndex = -1;
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if (!m_displayFrames.empty()) {
            numberofPendingFrames = (int)m_displayFrames.size();
            pictureIndex = m_displayFrames.front();
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameEncodeImageSet.size()));
            assert(!(m_ownedByDisplayMask & (1 << pictureIndex)));
            m_ownedByDisplayMask |= (1 << pictureIndex);
            m_displayFrames.pop();
            m_perFrameEncodeImageSet[pictureIndex].m_inDisplayQueue = false;
            m_perFrameEncodeImageSet[pictureIndex].m_ownedByDisplay = true;
        }

        if ((uint32_t)pictureIndex < m_perFrameEncodeImageSet.size()) {
            pEncodedFrame->pictureIndex = pictureIndex;

            pEncodedFrame->dpbImageView = m_perFrameEncodeImageSet[pictureIndex].GetFrameImageView();
            pEncodedFrame->inputImageView = m_perFrameEncodeImageSet[pictureIndex].GetDisplayImageView();

            pEncodedFrame->displayWidth  = m_perFrameEncodeImageSet[pictureIndex].m_picDispInfo.displayWidth;
            pEncodedFrame->displayHeight = m_perFrameEncodeImageSet[pictureIndex].m_picDispInfo.displayHeight;

            if (m_perFrameEncodeImageSet[pictureIndex].m_hasFrameCompleteSignalFence) {
                pEncodedFrame->frameCompleteFence = m_perFrameEncodeImageSet[pictureIndex].m_frameCompleteFence;
                m_perFrameEncodeImageSet[pictureIndex].m_hasFrameCompleteSignalFence = false;
            } else {
                pEncodedFrame->frameCompleteFence = VkFence();
            }

            if (m_perFrameEncodeImageSet[pictureIndex].m_hasFrameCompleteSignalSemaphore) {
                pEncodedFrame->frameCompleteSemaphore = m_perFrameEncodeImageSet[pictureIndex].m_frameCompleteSemaphore;
                m_perFrameEncodeImageSet[pictureIndex].m_hasFrameCompleteSignalSemaphore = false;
            } else {
                pEncodedFrame->frameCompleteSemaphore = VkSemaphore();
            }

            pEncodedFrame->frameConsumerDoneFence = m_perFrameEncodeImageSet[pictureIndex].m_frameConsumerDoneFence;
            pEncodedFrame->frameConsumerDoneSemaphore = m_perFrameEncodeImageSet[pictureIndex].m_frameConsumerDoneSemaphore;

            pEncodedFrame->timestamp = m_perFrameEncodeImageSet[pictureIndex].m_timestamp;
            pEncodedFrame->decodeOrder = m_perFrameEncodeImageSet[pictureIndex].m_decodeOrder;
            pEncodedFrame->displayOrder = m_perFrameEncodeImageSet[pictureIndex].m_displayOrder;

            pEncodedFrame->queryPool = m_queryPool;
            pEncodedFrame->startQueryId = pictureIndex;
            pEncodedFrame->numQueries = 1;
        }

        if (m_debug) {
            std::cout << "<<<<<<<<<<< Dequeue from Display: " << pictureIndex << " out of "
                      << numberofPendingFrames << " ===========" << std::endl;
        }
        return numberofPendingFrames;
    }

    virtual int32_t ReleaseDisplayedPicture(EncodedFrameRelease** pEncodedFramesRelease, uint32_t numFramesToRelease)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (uint32_t i = 0; i < numFramesToRelease; i++) {
            const EncodedFrameRelease* pEncodedFrameRelease = pEncodedFramesRelease[i];
            int picId = pEncodedFrameRelease->pictureIndex;
            assert((picId >= 0) && ((uint32_t)picId < m_perFrameEncodeImageSet.size()));

            assert(m_perFrameEncodeImageSet[picId].m_decodeOrder == pEncodedFrameRelease->decodeOrder);
            assert(m_perFrameEncodeImageSet[picId].m_displayOrder == pEncodedFrameRelease->displayOrder);

            assert(m_ownedByDisplayMask & (1 << picId));
            m_ownedByDisplayMask &= ~(1 << picId);
            m_perFrameEncodeImageSet[picId].m_inEncodeQueue = false;
            m_perFrameEncodeImageSet[picId].bitstreamData = nullptr;
            m_perFrameEncodeImageSet[picId].stdPps = nullptr;
            m_perFrameEncodeImageSet[picId].stdSps = nullptr;
            m_perFrameEncodeImageSet[picId].stdVps = nullptr;
            m_perFrameEncodeImageSet[picId].m_ownedByDisplay = false;
            m_perFrameEncodeImageSet[picId].Release();

            m_perFrameEncodeImageSet[picId].m_hasConsummerSignalFence = pEncodedFrameRelease->hasConsummerSignalFence;
            m_perFrameEncodeImageSet[picId].m_hasConsummerSignalSemaphore = pEncodedFrameRelease->hasConsummerSignalSemaphore;
        }
        return 0;
    }

    virtual int32_t GetDpbImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes,
                                                VkVideoPictureResourceInfoKHR* dpbPictureResources,
                                                PictureResourceInfo* dpbPictureResourcesInfo,
                                                VkImageLayout newDpbImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR)
    {
        assert(dpbPictureResources);
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)referenceSlotIndexes[resId] < m_perFrameEncodeImageSet.size()) {

                VkResult result = m_perFrameEncodeImageSet.GetImageSetNewLayout(m_vkDevCtx,
                                     referenceSlotIndexes[resId],
                                     newDpbImageLayerLayout,
                                     &dpbPictureResources[resId],
                                     &dpbPictureResourcesInfo[resId]);

                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    return -1;
                }

                assert(dpbPictureResources[resId].sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
                dpbPictureResources[resId].codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                dpbPictureResources[resId].codedExtent = m_codedExtent;
            }
        }
        return numResources;
    }

    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex,
                                                   VkVideoPictureResourceInfoKHR* dpbPictureResource,
                                                   PictureResourceInfo* dpbPictureResourceInfo,
                                                   VkImageLayout newDpbImageLayerLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                                                   VkVideoPictureResourceInfoKHR* inputPictureResource = nullptr,
                                                   PictureResourceInfo* inputPictureResourceInfo = nullptr,
                                                   VkImageLayout newInputImageLayerLayout = VK_IMAGE_LAYOUT_MAX_ENUM)
    {
        assert(dpbPictureResource);
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)referenceSlotIndex < m_perFrameEncodeImageSet.size()) {

            VkResult result = m_perFrameEncodeImageSet.GetImageSetNewLayout(m_vkDevCtx,
                                                                            referenceSlotIndex,
                                                                            newDpbImageLayerLayout,
                                                                            dpbPictureResource,
                                                                            dpbPictureResourceInfo,
                                                                            newInputImageLayerLayout,
                                                                            inputPictureResource,
                                                                            inputPictureResourceInfo);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }

            assert(dpbPictureResource->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
            dpbPictureResource->codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
            dpbPictureResource->codedExtent = m_codedExtent;

            if (inputPictureResource) {
                assert(inputPictureResource->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
                inputPictureResource->codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                inputPictureResource->codedExtent = m_codedExtent;
            }

        }
        return referenceSlotIndex;
    }

    virtual int32_t GetCurrentImageResourceByIndex(int8_t referenceSlotIndex,
                                                   VkSharedBaseObj<VkImageResourceView>& dpbImageView,
                                                   VkSharedBaseObj<VkImageResourceView>& inputImageView)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)referenceSlotIndex < m_perFrameEncodeImageSet.size()) {
            dpbImageView = m_perFrameEncodeImageSet[referenceSlotIndex].GetFrameImageView();
            inputImageView  = m_perFrameEncodeImageSet[referenceSlotIndex].GetDisplayImageView();
            return referenceSlotIndex;
        }
        return -1;
    }

    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)indexes[resId] < m_perFrameEncodeImageSet.size()) {
                m_perFrameEncodeImageSet[indexes[resId]].Deinit();
            }
        }
        return (int32_t)m_perFrameEncodeImageSet.size();
    }

    virtual uint64_t SetPicNumInEncodeOrder(int32_t picId, uint64_t picNumInEncodeOrder)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameEncodeImageSet.size()) {
            uint64_t oldPicNumInEncodeOrder = m_perFrameEncodeImageSet[picId].m_decodeOrder;
            m_perFrameEncodeImageSet[picId].m_decodeOrder = picNumInEncodeOrder;
            return oldPicNumInEncodeOrder;
        }
        assert(false);
        return (uint64_t)-1;
    }

    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameEncodeImageSet.size()) {
            int32_t oldPicNumInDisplayOrder = m_perFrameEncodeImageSet[picId].m_displayOrder;
            m_perFrameEncodeImageSet[picId].m_displayOrder = picNumInDisplayOrder;
            return oldPicNumInDisplayOrder;
        }
        assert(false);
        return -1;
    }

    virtual const VkSharedBaseObj<VkImageResourceView>& GetImageResourceByIndex(int8_t picId)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameEncodeImageSet.size()) {
            return m_perFrameEncodeImageSet[picId].GetFrameImageView();
        }
        assert(false);
        return emptyImageView;
    }

    virtual vkPicBuffBase* ReservePictureBuffer()
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        int32_t foundPicId = -1;
        for (uint32_t picId = 0; picId < m_perFrameEncodeImageSet.size(); picId++) {
            if (m_perFrameEncodeImageSet[picId].IsAvailable()) {
                foundPicId = picId;
		break;
            }
        }

        if (foundPicId >= 0) {
            m_perFrameEncodeImageSet[foundPicId].Reset();
            m_perFrameEncodeImageSet[foundPicId].AddRef();
            m_perFrameEncodeImageSet[foundPicId].m_picIdx = foundPicId;
            return &m_perFrameEncodeImageSet[foundPicId];
        }

        assert(foundPicId >= 0);
        return NULL;
    }

    virtual size_t GetSize()
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        return m_perFrameEncodeImageSet.size();
    }

    virtual ~VkVideoEncodeFrameBuffer()
    {
        Deinitialize();
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    std::atomic<int32_t>     m_refCount;
    std::mutex               m_displayQueueMutex;
    NvPerFrameEncodeImageSet m_perFrameEncodeImageSet;
    std::queue<uint8_t>      m_displayFrames;
    VkQueryPool              m_queryPool;
    uint32_t                 m_ownedByDisplayMask;
    int32_t                  m_frameNumInDisplayOrder;
    VkExtent2D               m_codedExtent;               // for the codedExtent, not the max image resolution
    uint32_t                 m_numberParameterUpdates;
    uint32_t                 m_debug : 1;
};

VkResult VulkanVideoEncodeFrameBuffer::Create(const VulkanDeviceContext* vkDevCtx,
                                        VkSharedBaseObj<VulkanVideoEncodeFrameBuffer>& vkVideoFrameBuffer)
{
    VkSharedBaseObj<VkVideoEncodeFrameBuffer> videoFrameBuffer(new VkVideoEncodeFrameBuffer(vkDevCtx));
    if (videoFrameBuffer) {
        vkVideoFrameBuffer = videoFrameBuffer;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

int32_t VkVideoEncodeFrameBuffer::AddRef()
{
    return ++m_refCount;
}

int32_t VkVideoEncodeFrameBuffer::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}

VkResult NvPerFrameEncodeResources::CreateImage( const VulkanDeviceContext* vkDevCtx,
                                                 const VkImageCreateInfo* pDpbImageCreateInfo,
                                                 const VkImageCreateInfo* pInImageCreateInfo,
                                                 VkMemoryPropertyFlags    dpbRequiredMemProps,
                                                 VkMemoryPropertyFlags    inRequiredMemProps,
                                                 uint32_t imageIndex,
                                                 VkSharedBaseObj<VkImageResource>& imageArrayParent,
                                                 VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                                                 bool useSeparateInputImage,
                                                 bool useLinearInput)
{
    VkResult result = VK_SUCCESS;

    if (!ImageExist() || m_recreateImage) {

        assert(m_vkDevCtx != nullptr);

        m_currentDpbImageLayerLayout = pDpbImageCreateInfo->initialLayout;
        m_currentInputImageLayout   = pInImageCreateInfo->initialLayout;

        VkSharedBaseObj<VkImageResource> imageResource;
        if (!imageArrayParent) {
            result = VkImageResource::Create(vkDevCtx,
                                             pDpbImageCreateInfo,
                                             dpbRequiredMemProps,
                                             imageResource);
            if (result != VK_SUCCESS) {
                return result;
            }
        } else {
            // We are using a parent array image
            imageResource = imageArrayParent;
        }

        if (!imageViewArrayParent) {

            uint32_t baseArrayLayer = imageArrayParent ? imageIndex : 0;
            VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseArrayLayer, 1 };
            result = VkImageResourceView::Create(vkDevCtx, imageResource,
                                                 subresourceRange,
                                                 m_frameDpbImageView);

            if (result != VK_SUCCESS) {
                return result;
            }

            if (!(useSeparateInputImage || useLinearInput)) {
                m_inImageView = m_frameDpbImageView;
            }

        } else {

            m_frameDpbImageView = imageViewArrayParent;

            if (!(useSeparateInputImage || useLinearInput)) {
                VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, imageIndex, 1 };
                result = VkImageResourceView::Create(vkDevCtx, imageResource,
                                                     subresourceRange,
                                                     m_inImageView);
                if (result != VK_SUCCESS) {
                    return result;
                }
            }
        }

        if (useSeparateInputImage || useLinearInput) {

            VkSharedBaseObj<VkImageResource> displayImageResource;
            result = VkImageResource::Create(vkDevCtx,
                                             pInImageCreateInfo,
                                             inRequiredMemProps,
                                             displayImageResource);
            if (result != VK_SUCCESS) {
                return result;
            }

            VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            result = VkImageResourceView::Create(vkDevCtx, displayImageResource,
                                                 subresourceRange,
                                                 m_inImageView);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
    }

    m_currentDpbImageLayerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_currentInputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_recreateImage = false;

    return result;
}

VkResult NvPerFrameEncodeResources::init(const VulkanDeviceContext* vkDevCtx)
{

    m_vkDevCtx = vkDevCtx;

    // The fence waited on for the first frame should be signaled.
    const VkFenceCreateInfo fenceFrameCompleteInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                                       VK_FENCE_CREATE_SIGNALED_BIT };
    VkResult result = m_vkDevCtx->CreateFence(*m_vkDevCtx, &fenceFrameCompleteInfo, nullptr, &m_frameCompleteFence);

    const VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr };
    result = m_vkDevCtx->CreateFence(*m_vkDevCtx, &fenceInfo, nullptr, &m_frameConsumerDoneFence);
    assert(result == VK_SUCCESS);

    const VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr };
    result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &semInfo, nullptr, &m_frameCompleteSemaphore);
    assert(result == VK_SUCCESS);
    result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &semInfo, nullptr, &m_frameConsumerDoneSemaphore);
    assert(result == VK_SUCCESS);

    Reset();

    return result;
}

void NvPerFrameEncodeResources::Deinit()
{
    bitstreamData = nullptr;
    stdPps = nullptr;
    stdSps = nullptr;
    stdVps = nullptr;

    if (m_vkDevCtx == nullptr) {
        assert ((m_frameCompleteFence == VK_NULL_HANDLE) &&
                (m_frameConsumerDoneFence == VK_NULL_HANDLE) &&
                (m_frameCompleteSemaphore == VK_NULL_HANDLE) &&
                (m_frameConsumerDoneSemaphore == VK_NULL_HANDLE) &&
                !m_frameDpbImageView &&
                !m_inImageView);
        return;
    }

    if (m_frameCompleteFence != VkFence()) {
        m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_frameCompleteFence, nullptr);
        m_frameCompleteFence = VkFence();
    }

    if (m_frameConsumerDoneFence != VkFence()) {
        m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_frameConsumerDoneFence, nullptr);
        m_frameConsumerDoneFence = VkFence();
    }

    if (m_frameCompleteSemaphore != VkSemaphore()) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_frameCompleteSemaphore, nullptr);
        m_frameCompleteSemaphore = VkSemaphore();
    }

    if (m_frameConsumerDoneSemaphore != VkSemaphore()) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_frameConsumerDoneSemaphore, nullptr);
        m_frameConsumerDoneSemaphore = VkSemaphore();
    }

    m_frameDpbImageView = nullptr;
    m_inImageView = nullptr;

    m_vkDevCtx = nullptr;

    Reset();
}

int32_t NvPerFrameEncodeImageSet::init(const VulkanDeviceContext* vkDevCtx,
                                       const VkVideoProfileInfoKHR* pEncodeProfile,
                                       uint32_t                 numImages,
                                       VkFormat                 dpbImageFormat,
                                       VkFormat                 inImageFormat,
                                       const VkExtent2D&        maxImageExtent,
                                       VkImageUsageFlags        dpbImageUsage,
                                       VkImageUsageFlags        inImageUsage,
                                       uint32_t                 queueFamilyIndex,
                                       VkMemoryPropertyFlags    dpbRequiredMemProps,
                                       VkMemoryPropertyFlags    inRequiredMemProps,
                                       bool                     useImageArray,
                                       bool                     useImageViewArray,
                                       bool                     useSeparateInputImage,
                                       bool                     useLinearInput)
{
    if (numImages > m_perFrameEncodeResources.size()) {
        assert(!"Number of requested images exceeds the max size of the image array");
        return -1;
    }

    const bool reconfigureImages = (m_numImages &&
        (m_dpbImageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_dpbImageCreateInfo.format != dpbImageFormat) ||
               (m_dpbImageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_dpbImageCreateInfo.extent.height < maxImageExtent.height));

    for (uint32_t imageIndex = m_numImages; imageIndex < numImages; imageIndex++) {
        VkResult result = m_perFrameEncodeResources[imageIndex].init(vkDevCtx);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            return -1;
        }
    }

    if (useImageViewArray) {
        useImageArray = true;
    }

    m_videoProfile.InitFromProfile(pEncodeProfile);

    m_queueFamilyIndex = queueFamilyIndex;
    m_dpbRequiredMemProps = dpbRequiredMemProps;
    m_inRequiredMemProps = inRequiredMemProps;

    // Image create info for the DPBs
    m_dpbImageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // m_imageCreateInfo.pNext = m_videoProfile.GetProfile();
    m_dpbImageCreateInfo.pNext = m_videoProfile.GetProfileListInfo();
    m_dpbImageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    m_dpbImageCreateInfo.format = dpbImageFormat;
    m_dpbImageCreateInfo.extent = { maxImageExtent.width, maxImageExtent.height, 1 };
    m_dpbImageCreateInfo.mipLevels = 1;
    m_dpbImageCreateInfo.arrayLayers = useImageArray ? numImages : 1;
    m_dpbImageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    m_dpbImageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    m_dpbImageCreateInfo.usage = dpbImageUsage;
    m_dpbImageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    m_dpbImageCreateInfo.queueFamilyIndexCount = 1;
    m_dpbImageCreateInfo.pQueueFamilyIndices = &m_queueFamilyIndex;
    m_dpbImageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_dpbImageCreateInfo.flags = 0;

    // Image create info for the input
    if (useSeparateInputImage) {
        m_inImageCreateInfo = m_dpbImageCreateInfo;
        m_inImageCreateInfo.format = inImageFormat;
        m_inImageCreateInfo.arrayLayers = 1;
        m_inImageCreateInfo.tiling = useLinearInput ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        m_inImageCreateInfo.usage = inImageUsage;

        if ((inImageUsage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0) {
            // A simple input image not directly used by the encoder
            m_inImageCreateInfo.pNext = nullptr;
        }
    }

    if (useImageArray) {
        // Create an image that has the same number of layers as the DPB images required.
        VkResult result = VkImageResource::Create(vkDevCtx,
                                                  &m_dpbImageCreateInfo,
                                                  m_dpbRequiredMemProps,
                                                  m_imageArray);
        if (result != VK_SUCCESS) {
            return -1;
        }
    } else {
        m_imageArray = nullptr;
    }

    if (useImageViewArray) {
        assert(m_imageArray);
        // Create an image view that has the same number of layers as the image.
        // In that scenario, while specifying the resource, the API must specifically choose the image layer.
        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, numImages };
        VkResult result = VkImageResourceView::Create(vkDevCtx, m_imageArray,
                                                      subresourceRange,
                                                      m_imageViewArray);

        if (result != VK_SUCCESS) {
            return -1;
        }
    }

    uint32_t firstIndex = reconfigureImages ? 0 : m_numImages;
    uint32_t maxNumImages = std::max(m_numImages, numImages);
    for (uint32_t imageIndex = firstIndex; imageIndex < maxNumImages; imageIndex++) {

        if (m_perFrameEncodeResources[imageIndex].ImageExist() && reconfigureImages) {

            m_perFrameEncodeResources[imageIndex].m_recreateImage = true;

        } else if (!m_perFrameEncodeResources[imageIndex].ImageExist()) {

            VkResult result =
                     m_perFrameEncodeResources[imageIndex].CreateImage(vkDevCtx,
                                                                  &m_dpbImageCreateInfo,
                                                                  &m_inImageCreateInfo,
                                                                  m_dpbRequiredMemProps,
                                                                  m_inRequiredMemProps,
                                                                  imageIndex,
                                                                  m_imageArray,
                                                                  m_imageViewArray,
                                                                  useSeparateInputImage,
                                                                  useLinearInput);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }
        }
    }

    m_numImages               = numImages;
    m_usesImageArray          = useImageArray;
    m_usesImageViewArray      = useImageViewArray;
    m_usesSeparateInputImage = useSeparateInputImage;
    m_usesLinearInput        = useLinearInput;

    return (int32_t)numImages;
}

void NvPerFrameEncodeImageSet::Deinit()
{
    for (size_t ndx = 0; ndx < m_numImages; ndx++) {
        m_perFrameEncodeResources[ndx].Deinit();
    }

    m_numImages = 0;
}
