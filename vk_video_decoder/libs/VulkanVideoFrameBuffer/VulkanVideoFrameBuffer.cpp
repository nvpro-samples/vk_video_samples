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
#include "PictureBufferBase.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VulkanVideoFrameBuffer.h"
#include "VkCodecUtils/VkImageResource.h"

static VkSharedBaseObj<VkImageResourceView> emptyImageView;

class NvPerFrameDecodeResources : public vkPicBuffBase {
public:
    NvPerFrameDecodeResources()
        : m_picDispInfo()
        , m_frameCompleteFence()
        , m_frameCompleteSemaphore()
        , m_frameConsumerDoneFence()
        , m_frameConsumerDoneSemaphore()
        , m_hasFrameCompleteSignalFence(false)
        , m_hasFrameCompleteSignalSemaphore(false)
        , m_hasConsummerSignalFence(false)
        , m_hasConsummerSignalSemaphore(false)
        , m_inDecodeQueue(false)
        , m_inDisplayQueue(false)
        , m_ownedByDisplay(false)
        , m_recreateImage(false)
        , m_currentDpbImageLayerLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_currentOutputImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_vkDevCtx()
        , m_frameDpbImageView()
        , m_outImageView()
    {
    }

    VkResult CreateImage( const VulkanDeviceContext* vkDevCtx,
                          const VkImageCreateInfo* pDpbImageCreateInfo,
                          const VkImageCreateInfo* pOutImageCreateInfo,
                          VkMemoryPropertyFlags    dpbRequiredMemProps,
                          VkMemoryPropertyFlags    outRequiredMemProps,
                          uint32_t imageIndex,
                          VkSharedBaseObj<VkImageResource>&  imageArrayParent,
                          VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                          bool useSeparateOutputImage = false,
                          bool useLinearOutput = false);

    VkResult init( const VulkanDeviceContext* vkDevCtx);

    void Deinit();

    NvPerFrameDecodeResources (const NvPerFrameDecodeResources &srcObj) = delete;
    NvPerFrameDecodeResources (NvPerFrameDecodeResources &&srcObj) = delete;

    ~NvPerFrameDecodeResources()
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
            return m_outImageView;
        } else {
            return emptyImageView;
        }
    }

    bool ImageExist() {

        return (!!m_frameDpbImageView && (m_frameDpbImageView->GetImageView() != VK_NULL_HANDLE));
    }

    bool GetImageSetNewLayout(VkImageLayout newDpbImageLayout,
                              VkVideoPictureResourceInfoKHR* pDpbPictureResource,
                              VulkanVideoFrameBuffer::PictureResourceInfo* pDpbPictureResourceInfo,
                              VkImageLayout newOutputImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM,
                              VkVideoPictureResourceInfoKHR* pOutputPictureResource = nullptr,
                              VulkanVideoFrameBuffer::PictureResourceInfo* pOutputPictureResourceInfo = nullptr) {


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

        if (pOutputPictureResourceInfo) {
            pOutputPictureResourceInfo->image = m_outImageView->GetImageResource()->GetImage();
            pOutputPictureResourceInfo->imageFormat = m_outImageView->GetImageResource()->GetImageCreateInfo().format;
            pOutputPictureResourceInfo->currentImageLayout = m_currentOutputImageLayout;
        }

        if (VK_IMAGE_LAYOUT_MAX_ENUM != newOutputImageLayout) {
            m_currentOutputImageLayout = newOutputImageLayout;
        }

        if (pOutputPictureResource) {
            pOutputPictureResource->imageViewBinding = m_outImageView->GetImageView();
        }

        return true;
    }

    VkParserDecodePictureInfo m_picDispInfo;
    VkFence m_frameCompleteFence;
    VkSemaphore m_frameCompleteSemaphore;
    VkFence m_frameConsumerDoneFence;
    VkSemaphore m_frameConsumerDoneSemaphore;
    uint32_t m_hasFrameCompleteSignalFence : 1;
    uint32_t m_hasFrameCompleteSignalSemaphore : 1;
    uint32_t m_hasConsummerSignalFence : 1;
    uint32_t m_hasConsummerSignalSemaphore : 1;
    uint32_t m_inDecodeQueue : 1;
    uint32_t m_inDisplayQueue : 1;
    uint32_t m_ownedByDisplay : 1;
    uint32_t m_recreateImage : 1;
    // VPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdVps;
    // SPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdSps;
    // PPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdPps;
    // AV1 SPS
    VkSharedBaseObj<VkVideoRefCountBase>  stdAv1Sps;
    // The bitstream Buffer
    VkSharedBaseObj<VkVideoRefCountBase>  bitstreamData;

private:
    VkImageLayout                        m_currentDpbImageLayerLayout;
    VkImageLayout                        m_currentOutputImageLayout;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<VkImageResourceView> m_frameDpbImageView;
    VkSharedBaseObj<VkImageResourceView> m_outImageView;
};

class NvPerFrameDecodeImageSet {
public:

    static constexpr size_t maxImages = 32;

    NvPerFrameDecodeImageSet()
        : m_queueFamilyIndex((uint32_t)-1)
        , m_dpbImageCreateInfo()
        , m_outImageCreateInfo()
        , m_dpbRequiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_outRequiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_numImages(0)
        , m_usesImageArray(false)
        , m_usesImageViewArray(false)
        , m_usesSeparateOutputImage(false)
        , m_usesLinearOutput(false)
        , m_perFrameDecodeResources(maxImages)
        , m_imageArray()
        , m_imageViewArray()
    {
    }

    int32_t init(const VulkanDeviceContext* vkDevCtx,
        const VkVideoProfileInfoKHR* pDecodeProfile,
        uint32_t              numImages,
        VkFormat              dpbImageFormat,
        VkFormat              outImageFormat,
        const VkExtent2D&     maxImageExtent,
        VkImageUsageFlags     dpbImageUsage,
        VkImageUsageFlags     outImageUsage,
        uint32_t              queueFamilyIndex,
        VkMemoryPropertyFlags dpbRequiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VkMemoryPropertyFlags outRequiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        bool useImageArray = false,
        bool useImageViewArray = false,
        bool useSeparateOutputImages = false,
        bool useLinearOutput = false);

    void Deinit();

    ~NvPerFrameDecodeImageSet()
    {
        Deinit();
    }

    NvPerFrameDecodeResources& operator[](unsigned int index)
    {
        assert(index < m_perFrameDecodeResources.size());
        return m_perFrameDecodeResources[index];
    }

    size_t size()
    {
        return m_numImages;
    }

    VkResult GetImageSetNewLayout(const VulkanDeviceContext* vkDevCtx,
                                  uint32_t imageIndex,
                                  VkImageLayout newDpbImageLayout,
                                  VkVideoPictureResourceInfoKHR* pDpbPictureResource = nullptr,
                                  VulkanVideoFrameBuffer::PictureResourceInfo* pDpbPictureResourceInfo = nullptr,
                                  VkImageLayout newOutputImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM,
                                  VkVideoPictureResourceInfoKHR* pOutputPictureResource = nullptr,
                                  VulkanVideoFrameBuffer::PictureResourceInfo* pOutputPictureResourceInfo = nullptr) {

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

        if(pOutputPictureResource) {
            // Output pictures currently are only allocated as discrete
            // Let the image view sub-resource specify the image layer.
            pOutputPictureResource->baseArrayLayer = 0;
        }

        bool validImage = m_perFrameDecodeResources[imageIndex].GetImageSetNewLayout(
                               newDpbImageLayout,
                               pDpbPictureResource,
                               pDpbPictureResourceInfo,
                               newOutputImageLayout,
                               pOutputPictureResource,
                               pOutputPictureResourceInfo);

        if (!validImage) {
            result = m_perFrameDecodeResources[imageIndex].CreateImage(
                               vkDevCtx,
                               &m_dpbImageCreateInfo,
                               &m_outImageCreateInfo,
                               m_dpbRequiredMemProps,
                               m_outRequiredMemProps,
                               imageIndex,
                               m_imageArray,
                               m_imageViewArray,
                               m_usesSeparateOutputImage,
                               m_usesLinearOutput);

            if (result == VK_SUCCESS) {
                validImage = m_perFrameDecodeResources[imageIndex].GetImageSetNewLayout(
                                               newDpbImageLayout,
                                               pDpbPictureResource,
                                               pDpbPictureResourceInfo,
                                               newOutputImageLayout,
                                               pOutputPictureResource,
                                               pOutputPictureResourceInfo);

                assert(validImage);
            }
        }

        return result;
    }

private:
    uint32_t                             m_queueFamilyIndex;
    VkVideoCoreProfile                   m_videoProfile;
    VkImageCreateInfo                    m_dpbImageCreateInfo;
    VkImageCreateInfo                    m_outImageCreateInfo;
    VkMemoryPropertyFlags                m_dpbRequiredMemProps;
    VkMemoryPropertyFlags                m_outRequiredMemProps;
    uint32_t                             m_numImages;
    uint32_t                             m_usesImageArray:1;
    uint32_t                             m_usesImageViewArray:1;
    uint32_t                             m_usesSeparateOutputImage:1;
    uint32_t                             m_usesLinearOutput:1;
    std::vector<NvPerFrameDecodeResources> m_perFrameDecodeResources;
    VkSharedBaseObj<VkImageResource>     m_imageArray;     // must be valid if m_usesImageArray is true
    VkSharedBaseObj<VkImageResourceView> m_imageViewArray; // must be valid if m_usesImageViewArray is true
};

class VkVideoFrameBuffer : public VulkanVideoFrameBuffer {
public:

    static constexpr size_t maxFramebufferImages = 32;

    VkVideoFrameBuffer(const VulkanDeviceContext* vkDevCtx)
        : m_vkDevCtx(vkDevCtx)
        , m_refCount(0)
        , m_displayQueueMutex()
        , m_perFrameDecodeImageSet()
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

    VkResult CreateVideoQueries(uint32_t numSlots, const VulkanDeviceContext* vkDevCtx, const VkVideoProfileInfoKHR* pDecodeProfile)
    {
        assert (numSlots <= maxFramebufferImages);

        if ((m_queryPool == VK_NULL_HANDLE) && m_vkDevCtx->GetVideoQueryResultStatusSupport()) {
            // It would be difficult to resize a query pool, so allocate the maximum possible slot.
            numSlots = maxFramebufferImages;
            VkQueryPoolCreateInfo queryPoolCreateInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryPoolCreateInfo.pNext = pDecodeProfile;
            queryPoolCreateInfo.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
            queryPoolCreateInfo.queryCount = numSlots; // m_numDecodeSurfaces frames worth

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
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameDecodeImageSet.size()));
            m_displayFrames.pop();
            if (m_perFrameDecodeImageSet[(uint32_t)pictureIndex].IsAvailable()) {
                // The frame is not released yet - force release it.
                m_perFrameDecodeImageSet[(uint32_t)pictureIndex].Release();
            }
            flushedImages++;
        }

        return flushedImages;
    }

    virtual int32_t InitImagePool(const VkVideoProfileInfoKHR* pDecodeProfile,
                                  uint32_t                 numImages,
                                  VkFormat                 dpbImageFormat,
                                  VkFormat                 outImageFormat,
                                  const VkExtent2D&        codedExtent,
                                  const VkExtent2D&        maxImageExtent,
                                  VkImageUsageFlags        dpbImageUsage,
                                  VkImageUsageFlags        outImageUsage,
                                  uint32_t                 queueFamilyIndex,
                                  int32_t                  numImagesToPreallocate,
                                  bool                     useImageArray = false,
                                  bool                     useImageViewArray = false,
                                  bool                     useSeparateOutputImage = false,
                                  bool                     useLinearOutput = false)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        assert(numImages && (numImages <= maxFramebufferImages) && pDecodeProfile);

        VkResult result = CreateVideoQueries(numImages, m_vkDevCtx, pDecodeProfile);
        if (result != VK_SUCCESS) {
            return 0;
        }

        // m_extent is for the codedExtent, not the max image resolution
        m_codedExtent = codedExtent;

        int32_t imageSetCreateResult =
                m_perFrameDecodeImageSet.init(m_vkDevCtx,
                                              pDecodeProfile,
                                              numImages,
                                              dpbImageFormat,
                                              outImageFormat,
                                              maxImageExtent,
                                              dpbImageUsage,
                                              outImageUsage,
                                              queueFamilyIndex,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              useLinearOutput ? ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT)  :
                                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              useImageArray, useImageViewArray,
                                              useSeparateOutputImage, useLinearOutput);
        m_numberParameterUpdates++;

        return imageSetCreateResult;
    }

    void Deinitialize() {

        FlushDisplayQueue();

        DestroyVideoQueries();

        m_ownedByDisplayMask = 0;
        m_frameNumInDisplayOrder = 0;

        m_perFrameDecodeImageSet.Deinit();

        if (m_queryPool != VkQueryPool()) {
            m_vkDevCtx->DestroyQueryPool(*m_vkDevCtx, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }
    };

    virtual int32_t QueueDecodedPictureForDisplay(int8_t picId, VulkanVideoDisplayPictureInfo* pDispInfo)
    {
        assert((uint32_t)picId < m_perFrameDecodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameDecodeImageSet[picId].m_displayOrder = m_frameNumInDisplayOrder++;
        m_perFrameDecodeImageSet[picId].m_timestamp = pDispInfo->timestamp;
        m_perFrameDecodeImageSet[picId].m_inDisplayQueue = true;
        m_perFrameDecodeImageSet[picId].AddRef();

        m_displayFrames.push((uint8_t)picId);

        if (m_debug) {
            std::cout << "==> Queue Display Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                      << "\ttimestamp " << m_perFrameDecodeImageSet[picId].m_timestamp << std::endl;
        }
        return picId;
    }

    virtual int32_t QueuePictureForDecode(int8_t picId, VkParserDecodePictureInfo* pDecodePictureInfo,
                                          ReferencedObjectsInfo* pReferencedObjectsInfo,
                                          FrameSynchronizationInfo* pFrameSynchronizationInfo)
    {
        assert((uint32_t)picId < m_perFrameDecodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameDecodeImageSet[picId].m_picDispInfo = *pDecodePictureInfo;
        m_perFrameDecodeImageSet[picId].m_inDecodeQueue = true;
        m_perFrameDecodeImageSet[picId].stdPps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdPps);
        m_perFrameDecodeImageSet[picId].stdSps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdSps);
        m_perFrameDecodeImageSet[picId].stdVps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdVps);
        m_perFrameDecodeImageSet[picId].stdAv1Sps = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pStdAv1Sps);
        m_perFrameDecodeImageSet[picId].bitstreamData = const_cast<VkVideoRefCountBase*>(pReferencedObjectsInfo->pBitstreamData);

        if (m_debug) {
            std::cout << "==> Queue Decode Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                      << "\tFrameType " << m_perFrameDecodeImageSet[picId].m_picDispInfo.videoFrameType << std::endl;
        }

        if (pFrameSynchronizationInfo->hasFrameCompleteSignalFence) {
            pFrameSynchronizationInfo->frameCompleteFence = m_perFrameDecodeImageSet[picId].m_frameCompleteFence;
            if (pFrameSynchronizationInfo->frameCompleteFence) {
                m_perFrameDecodeImageSet[picId].m_hasFrameCompleteSignalFence = true;
            }
        }

        if (m_perFrameDecodeImageSet[picId].m_hasConsummerSignalFence) {
            pFrameSynchronizationInfo->frameConsumerDoneFence = m_perFrameDecodeImageSet[picId].m_frameConsumerDoneFence;
            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalFence = false;
        }

        if (pFrameSynchronizationInfo->hasFrameCompleteSignalSemaphore) {
            pFrameSynchronizationInfo->frameCompleteSemaphore = m_perFrameDecodeImageSet[picId].m_frameCompleteSemaphore;
            if (pFrameSynchronizationInfo->frameCompleteSemaphore) {
                m_perFrameDecodeImageSet[picId].m_hasFrameCompleteSignalSemaphore = true;
            }
        }

        if (m_perFrameDecodeImageSet[picId].m_hasConsummerSignalSemaphore) {
            pFrameSynchronizationInfo->frameConsumerDoneSemaphore = m_perFrameDecodeImageSet[picId].m_frameConsumerDoneSemaphore;
            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalSemaphore = false;
        }

        pFrameSynchronizationInfo->queryPool = m_queryPool;
        pFrameSynchronizationInfo->startQueryId = picId;
        pFrameSynchronizationInfo->numQueries = 1;

        return picId;
    }

    // dequeue
    virtual int32_t DequeueDecodedPicture(DecodedFrame* pDecodedFrame)
    {
        int numberofPendingFrames = 0;
        int pictureIndex = -1;
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if (!m_displayFrames.empty()) {
            numberofPendingFrames = (int)m_displayFrames.size();
            pictureIndex = m_displayFrames.front();
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameDecodeImageSet.size()));
            assert(!(m_ownedByDisplayMask & (1 << pictureIndex)));
            m_ownedByDisplayMask |= (1 << pictureIndex);
            m_displayFrames.pop();
            m_perFrameDecodeImageSet[pictureIndex].m_inDisplayQueue = false;
            m_perFrameDecodeImageSet[pictureIndex].m_ownedByDisplay = true;
        }

        if ((uint32_t)pictureIndex < m_perFrameDecodeImageSet.size()) {
            pDecodedFrame->pictureIndex = pictureIndex;

            pDecodedFrame->decodedImageView = m_perFrameDecodeImageSet[pictureIndex].GetFrameImageView();
            pDecodedFrame->outputImageView = m_perFrameDecodeImageSet[pictureIndex].GetDisplayImageView();

            pDecodedFrame->displayWidth  = m_perFrameDecodeImageSet[pictureIndex].m_picDispInfo.displayWidth;
            pDecodedFrame->displayHeight = m_perFrameDecodeImageSet[pictureIndex].m_picDispInfo.displayHeight;

            if (m_perFrameDecodeImageSet[pictureIndex].m_hasFrameCompleteSignalFence) {
                pDecodedFrame->frameCompleteFence = m_perFrameDecodeImageSet[pictureIndex].m_frameCompleteFence;
                m_perFrameDecodeImageSet[pictureIndex].m_hasFrameCompleteSignalFence = false;
            } else {
                pDecodedFrame->frameCompleteFence = VkFence();
            }

            if (m_perFrameDecodeImageSet[pictureIndex].m_hasFrameCompleteSignalSemaphore) {
                pDecodedFrame->frameCompleteSemaphore = m_perFrameDecodeImageSet[pictureIndex].m_frameCompleteSemaphore;
                m_perFrameDecodeImageSet[pictureIndex].m_hasFrameCompleteSignalSemaphore = false;
            } else {
                pDecodedFrame->frameCompleteSemaphore = VkSemaphore();
            }

            pDecodedFrame->frameConsumerDoneFence = m_perFrameDecodeImageSet[pictureIndex].m_frameConsumerDoneFence;
            pDecodedFrame->frameConsumerDoneSemaphore = m_perFrameDecodeImageSet[pictureIndex].m_frameConsumerDoneSemaphore;

            pDecodedFrame->timestamp = m_perFrameDecodeImageSet[pictureIndex].m_timestamp;
            pDecodedFrame->decodeOrder = m_perFrameDecodeImageSet[pictureIndex].m_decodeOrder;
            pDecodedFrame->displayOrder = m_perFrameDecodeImageSet[pictureIndex].m_displayOrder;

            pDecodedFrame->queryPool = m_queryPool;
            pDecodedFrame->startQueryId = pictureIndex;
            pDecodedFrame->numQueries = 1;
        }

        if (m_debug) {
            std::cout << "<<<<<<<<<<< Dequeue from Display: " << pictureIndex << " out of "
                      << numberofPendingFrames << " ===========" << std::endl;
        }
        return numberofPendingFrames;
    }

    virtual int32_t ReleaseDisplayedPicture(DecodedFrameRelease** pDecodedFramesRelease, uint32_t numFramesToRelease)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (uint32_t i = 0; i < numFramesToRelease; i++) {
            const DecodedFrameRelease* pDecodedFrameRelease = pDecodedFramesRelease[i];
            int picId = pDecodedFrameRelease->pictureIndex;
            assert((picId >= 0) && ((uint32_t)picId < m_perFrameDecodeImageSet.size()));

            assert(m_perFrameDecodeImageSet[picId].m_decodeOrder == pDecodedFrameRelease->decodeOrder);
            assert(m_perFrameDecodeImageSet[picId].m_displayOrder == pDecodedFrameRelease->displayOrder);

            assert(m_ownedByDisplayMask & (1 << picId));
            m_ownedByDisplayMask &= ~(1 << picId);
            m_perFrameDecodeImageSet[picId].m_inDecodeQueue = false;
            m_perFrameDecodeImageSet[picId].bitstreamData = nullptr;
            m_perFrameDecodeImageSet[picId].stdPps = nullptr;
            m_perFrameDecodeImageSet[picId].stdSps = nullptr;
            m_perFrameDecodeImageSet[picId].stdVps = nullptr;
            m_perFrameDecodeImageSet[picId].stdAv1Sps = nullptr;
            m_perFrameDecodeImageSet[picId].m_ownedByDisplay = false;
            m_perFrameDecodeImageSet[picId].Release();

            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalFence = pDecodedFrameRelease->hasConsummerSignalFence;
            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalSemaphore = pDecodedFrameRelease->hasConsummerSignalSemaphore;
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
            if ((uint32_t)referenceSlotIndexes[resId] < m_perFrameDecodeImageSet.size()) {

                VkResult result = m_perFrameDecodeImageSet.GetImageSetNewLayout(m_vkDevCtx,
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
                                                   VkVideoPictureResourceInfoKHR* outputPictureResource = nullptr,
                                                   PictureResourceInfo* outputPictureResourceInfo = nullptr,
                                                   VkImageLayout newOutputImageLayerLayout = VK_IMAGE_LAYOUT_MAX_ENUM)
    {
        assert(dpbPictureResource);
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)referenceSlotIndex < m_perFrameDecodeImageSet.size()) {

            VkResult result = m_perFrameDecodeImageSet.GetImageSetNewLayout(m_vkDevCtx,
                                                                            referenceSlotIndex,
                                                                            newDpbImageLayerLayout,
                                                                            dpbPictureResource,
                                                                            dpbPictureResourceInfo,
                                                                            newOutputImageLayerLayout,
                                                                            outputPictureResource,
                                                                            outputPictureResourceInfo);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }

            assert(dpbPictureResource->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
            dpbPictureResource->codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
            dpbPictureResource->codedExtent = m_codedExtent;

            if (outputPictureResource) {
                assert(outputPictureResource->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
                outputPictureResource->codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                outputPictureResource->codedExtent = m_codedExtent;
            }

        }
        return referenceSlotIndex;
    }

    virtual int32_t ReleaseImageResources(uint32_t numResources, const uint32_t* indexes)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)indexes[resId] < m_perFrameDecodeImageSet.size()) {
                m_perFrameDecodeImageSet[indexes[resId]].Deinit();
            }
        }
        return (int32_t)m_perFrameDecodeImageSet.size();
    }

    virtual uint64_t SetPicNumInDecodeOrder(int32_t picId, uint64_t picNumInDecodeOrder)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameDecodeImageSet.size()) {
            uint64_t oldPicNumInDecodeOrder = m_perFrameDecodeImageSet[picId].m_decodeOrder;
            m_perFrameDecodeImageSet[picId].m_decodeOrder = picNumInDecodeOrder;
            return oldPicNumInDecodeOrder;
        }
        assert(false);
        return (uint64_t)-1;
    }

    virtual int32_t SetPicNumInDisplayOrder(int32_t picId, int32_t picNumInDisplayOrder)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameDecodeImageSet.size()) {
            int32_t oldPicNumInDisplayOrder = m_perFrameDecodeImageSet[picId].m_displayOrder;
            m_perFrameDecodeImageSet[picId].m_displayOrder = picNumInDisplayOrder;
            return oldPicNumInDisplayOrder;
        }
        assert(false);
        return -1;
    }

    virtual const VkSharedBaseObj<VkImageResourceView>& GetImageResourceByIndex(int8_t picId)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameDecodeImageSet.size()) {
            return m_perFrameDecodeImageSet[picId].GetFrameImageView();
        }
        assert(false);
        return emptyImageView;
    }

    virtual vkPicBuffBase* ReservePictureBuffer()
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        int32_t foundPicId = -1;
        for (uint32_t picId = 0; picId < m_perFrameDecodeImageSet.size(); picId++) {
            if (m_perFrameDecodeImageSet[picId].IsAvailable()) {
                foundPicId = picId;
		break;
            }
        }

        if (foundPicId >= 0) {
            m_perFrameDecodeImageSet[foundPicId].Reset();
            m_perFrameDecodeImageSet[foundPicId].AddRef();
            m_perFrameDecodeImageSet[foundPicId].m_picIdx = foundPicId;
            return &m_perFrameDecodeImageSet[foundPicId];
        }

        assert(foundPicId >= 0);
        return NULL;
    }

    virtual size_t GetSize()
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        return m_perFrameDecodeImageSet.size();
    }

    virtual ~VkVideoFrameBuffer()
    {
        Deinitialize();
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    std::atomic<int32_t>     m_refCount;
    std::mutex               m_displayQueueMutex;
    NvPerFrameDecodeImageSet m_perFrameDecodeImageSet;
    std::queue<uint8_t>      m_displayFrames;
    VkQueryPool              m_queryPool;
    uint32_t                 m_ownedByDisplayMask;
    int32_t                  m_frameNumInDisplayOrder;
    VkExtent2D               m_codedExtent;               // for the codedExtent, not the max image resolution
    uint32_t                 m_numberParameterUpdates;
    uint32_t                 m_debug : 1;
};

VkResult VulkanVideoFrameBuffer::Create(const VulkanDeviceContext* vkDevCtx,
                                        VkSharedBaseObj<VulkanVideoFrameBuffer>& vkVideoFrameBuffer)
{
    VkSharedBaseObj<VkVideoFrameBuffer> videoFrameBuffer(new VkVideoFrameBuffer(vkDevCtx));
    if (videoFrameBuffer) {
        vkVideoFrameBuffer = videoFrameBuffer;
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

int32_t VkVideoFrameBuffer::AddRef()
{
    return ++m_refCount;
}

int32_t VkVideoFrameBuffer::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}

VkResult NvPerFrameDecodeResources::CreateImage( const VulkanDeviceContext* vkDevCtx,
                                                 const VkImageCreateInfo* pDpbImageCreateInfo,
                                                 const VkImageCreateInfo* pOutImageCreateInfo,
                                                 VkMemoryPropertyFlags    dpbRequiredMemProps,
                                                 VkMemoryPropertyFlags    outRequiredMemProps,
                                                 uint32_t imageIndex,
                                                 VkSharedBaseObj<VkImageResource>& imageArrayParent,
                                                 VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                                                 bool useSeparateOutputImage,
                                                 bool useLinearOutput)
{
    VkResult result = VK_SUCCESS;

    if (!ImageExist() || m_recreateImage) {

        assert(m_vkDevCtx != nullptr);

        m_currentDpbImageLayerLayout = pDpbImageCreateInfo->initialLayout;
        m_currentOutputImageLayout   = pOutImageCreateInfo->initialLayout;

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

            if (!(useSeparateOutputImage || useLinearOutput)) {
                m_outImageView = m_frameDpbImageView;
            }

        } else {

            m_frameDpbImageView = imageViewArrayParent;

            if (!(useSeparateOutputImage || useLinearOutput)) {
                VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, imageIndex, 1 };
                result = VkImageResourceView::Create(vkDevCtx, imageResource,
                                                     subresourceRange,
                                                     m_outImageView);
                if (result != VK_SUCCESS) {
                    return result;
                }
            }
        }

        if (useSeparateOutputImage || useLinearOutput) {

            VkSharedBaseObj<VkImageResource> displayImageResource;
            result = VkImageResource::Create(vkDevCtx,
                                             pOutImageCreateInfo,
                                             outRequiredMemProps,
                                             displayImageResource);
            if (result != VK_SUCCESS) {
                return result;
            }

            VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            result = VkImageResourceView::Create(vkDevCtx, displayImageResource,
                                                 subresourceRange,
                                                 m_outImageView);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
    }

    m_currentDpbImageLayerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_currentOutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_recreateImage = false;

    return result;
}

VkResult NvPerFrameDecodeResources::init(const VulkanDeviceContext* vkDevCtx)
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

void NvPerFrameDecodeResources::Deinit()
{
    bitstreamData = nullptr;
    stdPps = nullptr;
    stdSps = nullptr;
    stdVps = nullptr;
    stdAv1Sps = nullptr;

    if (m_vkDevCtx == nullptr) {
        assert ((m_frameCompleteFence == VK_NULL_HANDLE) &&
                (m_frameConsumerDoneFence == VK_NULL_HANDLE) &&
                (m_frameCompleteSemaphore == VK_NULL_HANDLE) &&
                (m_frameConsumerDoneSemaphore == VK_NULL_HANDLE) &&
                !m_frameDpbImageView &&
                !m_outImageView);
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
    m_outImageView = nullptr;

    m_vkDevCtx = nullptr;

    Reset();
}

int32_t NvPerFrameDecodeImageSet::init(const VulkanDeviceContext* vkDevCtx,
                                       const VkVideoProfileInfoKHR* pDecodeProfile,
                                       uint32_t                 numImages,
                                       VkFormat                 dpbImageFormat,
                                       VkFormat                 outImageFormat,
                                       const VkExtent2D&        maxImageExtent,
                                       VkImageUsageFlags        dpbImageUsage,
                                       VkImageUsageFlags        outImageUsage,
                                       uint32_t                 queueFamilyIndex,
                                       VkMemoryPropertyFlags    dpbRequiredMemProps,
                                       VkMemoryPropertyFlags    outRequiredMemProps,
                                       bool                     useImageArray,
                                       bool                     useImageViewArray,
                                       bool                     useSeparateOutputImage,
                                       bool                     useLinearOutput)
{
    if (numImages > m_perFrameDecodeResources.size()) {
        assert(!"Number of requested images exceeds the max size of the image array");
        return -1;
    }

    const bool reconfigureImages = (m_numImages &&
        (m_dpbImageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_dpbImageCreateInfo.format != dpbImageFormat) ||
               (m_dpbImageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_dpbImageCreateInfo.extent.height < maxImageExtent.height));

    for (uint32_t imageIndex = m_numImages; imageIndex < numImages; imageIndex++) {
        VkResult result = m_perFrameDecodeResources[imageIndex].init(vkDevCtx);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            return -1;
        }
    }

    if (useImageViewArray) {
        useImageArray = true;
    }

    m_videoProfile.InitFromProfile(pDecodeProfile);

    m_queueFamilyIndex = queueFamilyIndex;
    m_dpbRequiredMemProps = dpbRequiredMemProps;
    m_outRequiredMemProps = outRequiredMemProps;

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

    // Image create info for the output
    if (useSeparateOutputImage) {
        m_outImageCreateInfo = m_dpbImageCreateInfo;
        m_outImageCreateInfo.format = outImageFormat;
        m_outImageCreateInfo.arrayLayers = 1;
        m_outImageCreateInfo.tiling = useLinearOutput ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        m_outImageCreateInfo.usage = outImageUsage;

        if ((outImageUsage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0) {
            // A simple output image not directly used by the decoder
            m_outImageCreateInfo.pNext = nullptr;
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

        if (m_perFrameDecodeResources[imageIndex].ImageExist() && reconfigureImages) {

            m_perFrameDecodeResources[imageIndex].m_recreateImage = true;

        } else if (!m_perFrameDecodeResources[imageIndex].ImageExist()) {

            VkResult result =
                     m_perFrameDecodeResources[imageIndex].CreateImage(vkDevCtx,
                                                                  &m_dpbImageCreateInfo,
                                                                  &m_outImageCreateInfo,
                                                                  m_dpbRequiredMemProps,
                                                                  m_outRequiredMemProps,
                                                                  imageIndex,
                                                                  m_imageArray,
                                                                  m_imageViewArray,
                                                                  useSeparateOutputImage,
                                                                  useLinearOutput);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }
        }
    }

    m_numImages               = numImages;
    m_usesImageArray          = useImageArray;
    m_usesImageViewArray      = useImageViewArray;
    m_usesSeparateOutputImage = useSeparateOutputImage;
    m_usesLinearOutput        = useLinearOutput;

    return (int32_t)numImages;
}

void NvPerFrameDecodeImageSet::Deinit()
{
    for (size_t ndx = 0; ndx < m_numImages; ndx++) {
        m_perFrameDecodeResources[ndx].Deinit();
    }

    m_numImages = 0;
}
