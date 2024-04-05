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
static VkSharedBaseObj<VkImageResource>     emptyImage;

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
        , m_frameUsesFg(false)
        , m_recreateImage{false,false,false}
        , m_currentDpbImageLayerLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_currentOutputImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
        , m_vkDevCtx()
        , m_frameDpbImageView()
        , m_outImageView()
        , m_outLinearImage()
    {
    }

    VkResult CreateImage( const VulkanDeviceContext* vkDevCtx,
                          const VkImageCreateInfo* pDpbImageCreateInfo,
                          //const VkImageCreateInfo* pOutImageCreateInfo,
                          //const VkImageCreateInfo* pOutLinearImageCreateInfo,
                          VkMemoryPropertyFlags    dpbRequiredMemProps,
                          //VkMemoryPropertyFlags    outRequiredMemProps,
                          uint32_t imageIndex,
                          VkSharedBaseObj<VkImageResource>&  imageArrayParent,
                          VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                          ImageType imageType);

    VkResult init( const VulkanDeviceContext* vkDevCtx);

    void Deinit();

    NvPerFrameDecodeResources (const NvPerFrameDecodeResources &srcObj) = delete;
    NvPerFrameDecodeResources (NvPerFrameDecodeResources &&srcObj) = delete;

    ~NvPerFrameDecodeResources()
    {
        Deinit();
    }

    VkSharedBaseObj<VkImageResourceView>& GetDisplayImageView() {
        if (ImageExist(PresentableImage)) {
            return m_outImageView;
        } else if (ImageExist(ReferenceableImage)) {
            return m_frameDpbImageView;
        } else {
            assert(false);
            return emptyImageView;
        }
    }

    VkSharedBaseObj<VkImageResource>& GetLinearImage() {
        if (ImageExist(HostVisibleImage)) {
            return m_outLinearImage;
        } else {
            return emptyImage;
        }
    }

    bool ImageExist(ImageType imageType) {
        if (imageType == ReferenceableImage) {
            return (!!m_frameDpbImageView && (m_frameDpbImageView->GetImageView() != VK_NULL_HANDLE));
        } else if (imageType == PresentableImage) {
            return (!!m_outImageView && (m_outImageView->GetImageView() != VK_NULL_HANDLE));
        } else if (imageType == HostVisibleImage) {
            return (!!m_outLinearImage && (m_outLinearImage != VK_NULL_HANDLE));
        }
        return false;
    }

    bool GetImage(VkVideoPictureResourceInfoKHR* pPictureResource,
                  VulkanVideoFrameBuffer::PictureResourceInfo* pPictureResourceInfo,
                  ImageType imageType) {

        VkImage image = 0;
        VkImageView imageView = 0;
        VkFormat imageFormat = VK_FORMAT_UNDEFINED;
        VkImageLayout currentImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
        if ((m_recreateImage[imageType] != false) || (ImageExist(imageType) == false)) {
            return false;
        }

        switch (imageType) {
            case ReferenceableImage:
                image = m_frameDpbImageView->GetImageResource()->GetImage();
                imageFormat = m_frameDpbImageView->GetImageResource()->GetImageCreateInfo().format;
                imageView = m_frameDpbImageView->GetImageView();
                currentImageLayout = m_currentDpbImageLayerLayout;
            break;
            case PresentableImage:
                image = m_outImageView->GetImageResource()->GetImage();
                imageFormat = m_outImageView->GetImageResource()->GetImageCreateInfo().format;
                imageView = m_outImageView->GetImageView();
                currentImageLayout = m_currentOutputImageLayout;
            break;
            case HostVisibleImage:
                image = m_outLinearImage->GetImage();
            break;
            default:
                assert(false);
        }
        
        if (pPictureResourceInfo != nullptr) {
            pPictureResourceInfo->image = image;
            if (imageFormat != VK_FORMAT_UNDEFINED) {
                pPictureResourceInfo->imageFormat = imageFormat;
                pPictureResourceInfo->currentImageLayout = currentImageLayout;
                pPictureResource->imageViewBinding = imageView;
            }
        }

        return true;
    }

    bool SetLayout(VkImageLayout newImageLayout, ImageType imageType) {
        if (VK_IMAGE_LAYOUT_MAX_ENUM == newImageLayout) {
            return false;
        }

        switch (imageType) {
            case ReferenceableImage:
                m_currentDpbImageLayerLayout = newImageLayout;
            break;
            case PresentableImage:
                m_currentOutputImageLayout = newImageLayout;
            break;
            case HostVisibleImage:
            break;
            default:
                assert(false);
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
    uint32_t m_frameUsesFg : 1;

    bool m_recreateImage[ImageTypeMAX];
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
    // TODO: Ideally it should go one level up and the NvPerFrameDecodeResources should keep track of one type of resource.
    // Another alternative is an array of sharedBaseObj<VkImageResourceView>[3]
    // 
    VkImageLayout                        m_currentDpbImageLayerLayout;
    VkImageLayout                        m_currentOutputImageLayout;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<VkImageResourceView> m_frameDpbImageView;
    VkSharedBaseObj<VkImageResourceView> m_outImageView;
    VkSharedBaseObj<VkImageResource>     m_outLinearImage;
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
        , m_linearRequiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , m_numImages{0,0,0}
        , m_usesImageArray(false)
        , m_usesImageViewArray(false)
        , m_perFrameDecodeResources(maxImages)
        , m_imageArray()
        , m_imageViewArray()
    {
    }

    int32_t initDpbImages(const VulkanDeviceContext* vkDevCtx,
                          const VkVideoProfileInfoKHR* pDecodeProfile,
                          uint32_t                 numImages,
                          VkFormat                 dpbImageFormat,
                          const VkExtent2D&        maxImageExtent,
                          VkImageUsageFlags        dpbImageUsage,
                          uint32_t                 queueFamilyIndex,
                          VkMemoryPropertyFlags    dpbRequiredMemProps,
                          bool                     useImageArray,
                          bool                     useImageViewArray);

     int32_t initOutputImages(const VulkanDeviceContext* vkDevCtx,
                              const VkVideoProfileInfoKHR* pDecodeProfile,
                              uint32_t                 numImages,
                              VkFormat                 dpbImageFormat,
                              const VkExtent2D&        maxImageExtent,
                              VkImageUsageFlags        dpbImageUsage,
                              uint32_t                 queueFamilyIndex,
                              VkMemoryPropertyFlags    dpbRequiredMemProps,
                              bool                     useImageArray,
                              bool                     useImageViewArray);

     int32_t initLinearImages(const VulkanDeviceContext* vkDevCtx,
                              const VkVideoProfileInfoKHR* pDecodeProfile,
                              uint32_t                 numImages,
                              VkFormat                 dpbImageFormat,
                              const VkExtent2D&        maxImageExtent,
                              VkImageUsageFlags        dpbImageUsage,
                              uint32_t                 queueFamilyIndex,
                              VkMemoryPropertyFlags    dpbRequiredMemProps,
                              bool                     useImageArray,
                              bool                     useImageViewArray);
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
        return m_numImages[0];
    }

    VkResult GetImage(const VulkanDeviceContext* vkDevCtx,
                      uint32_t imageIndex,
                      VkVideoPictureResourceInfoKHR* pPictureResource,
                      VulkanVideoFrameBuffer::PictureResourceInfo* pPictureResourceInfo,
                      ImageType imageType)
    {
        if (pPictureResource == nullptr) {
            return VK_SUCCESS; // Weird.
        }

        VkResult result = VK_SUCCESS;
        if (imageType == ReferenceableImage) {
            if (m_imageViewArray != nullptr) {
                // We have an image view that has the same number of layers as the image.
                // In that scenario, while specifying the resource, the API must specifically choose the image layer.
                pPictureResource->baseArrayLayer = imageIndex;
            } else {
                // Let the image view sub-resource specify the image layer.
                pPictureResource->baseArrayLayer = 0;
            }

        } else if (imageType == PresentableImage) {
            // Output pictures currently are only allocated as discrete
            // Let the image view sub-resource specify the image layer.
            pPictureResource->baseArrayLayer = 0;
        }

        VkSharedBaseObj<VkImageResource> nullImageArray;
        VkSharedBaseObj<VkImageResourceView> nullViewArray;
        NvPerFrameDecodeResources& decodeResource = m_perFrameDecodeResources[imageIndex];
        bool validImage = decodeResource.GetImage(pPictureResource, pPictureResourceInfo, imageType);
        if (validImage == false) {
            switch (imageType) {
                case ReferenceableImage: {
                    result = decodeResource.CreateImage(vkDevCtx, &m_dpbImageCreateInfo, m_dpbRequiredMemProps, imageIndex, m_imageArray, m_imageViewArray, imageType);
                    if (m_usesImageArray) {
                        result = decodeResource.CreateImage(vkDevCtx, &m_outImageCreateInfo, m_outRequiredMemProps, imageIndex, m_imageArray, nullViewArray, PresentableImage);
                    }
                }
                break;
                case PresentableImage:
                    result = decodeResource.CreateImage(vkDevCtx, &m_outImageCreateInfo, m_outRequiredMemProps, imageIndex, nullImageArray, nullViewArray, imageType);
                break;
                case HostVisibleImage:
                    result = decodeResource.CreateImage(vkDevCtx, &m_linearOutImageCreateInfo, m_linearRequiredMemProps, imageIndex, nullImageArray, nullViewArray, imageType);
                break;
                default:
                    assert(!"Invalid picture type");
                    break;
            }

            if (result == VK_SUCCESS) {
                validImage = decodeResource.GetImage(pPictureResource, pPictureResourceInfo, imageType);
                assert(validImage != false);
            }
        }

        return result;
    }

    VkResult SetNewLayout(const VulkanDeviceContext*,
                          uint32_t imageIndex,
                          VkImageLayout newImageLayout,
                          ImageType imageType)
    {
        NvPerFrameDecodeResources& decodeResource = m_perFrameDecodeResources[imageIndex];
        decodeResource.SetLayout(newImageLayout, imageType);
        return VK_SUCCESS;
    }

private:
    uint32_t                             m_queueFamilyIndex;
    VkVideoCoreProfile                   m_videoProfile;
    VkImageCreateInfo                    m_dpbImageCreateInfo;
    VkImageCreateInfo                    m_outImageCreateInfo;
    VkImageCreateInfo                    m_linearOutImageCreateInfo;
    VkMemoryPropertyFlags                m_dpbRequiredMemProps;
    VkMemoryPropertyFlags                m_outRequiredMemProps;
    VkMemoryPropertyFlags                m_linearRequiredMemProps;
    uint32_t                             m_numImages[ImageTypeMAX];
    uint32_t                             m_usesImageArray:1;
    uint32_t                             m_usesImageViewArray:1;
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
                                  const VkExtent2D&        codedExtent,
                                  const VkExtent2D&        maxImageExtent,
                                  VkImageUsageFlags        dpbImageUsage,
                                  uint32_t                 queueFamilyIndex,
                                  int32_t                                  ,
                                  ImageType                imageType,
                                  bool                     useImageArray = false,
                                  bool                     useImageViewArray = false)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        assert(numImages && (numImages <= maxFramebufferImages) && pDecodeProfile);

        int32_t imageSetCreateResult = VK_SUCCESS;
        if (imageType == ReferenceableImage) {
            VkResult result = CreateVideoQueries(numImages, m_vkDevCtx, pDecodeProfile);
            if (result != VK_SUCCESS) {
                return 0;
            }

            // m_extent is for the codedExtent, not the max image resolution
            m_codedExtent = codedExtent;
            imageSetCreateResult = m_perFrameDecodeImageSet.initDpbImages(m_vkDevCtx,
                                                    pDecodeProfile,
                                                    numImages,
                                                    dpbImageFormat,
                                                    maxImageExtent,
                                                    dpbImageUsage,
                                                    queueFamilyIndex,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    useImageArray, useImageViewArray);

            m_numberParameterUpdates++;
        } else if (imageType == PresentableImage) {
            imageSetCreateResult = m_perFrameDecodeImageSet.initOutputImages(m_vkDevCtx,
                                        pDecodeProfile,
                                        numImages,
                                        dpbImageFormat,
                                        maxImageExtent,
                                        dpbImageUsage,
                                        queueFamilyIndex,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        useImageArray, useImageViewArray);

        } else if (imageType == HostVisibleImage) {
            imageSetCreateResult = m_perFrameDecodeImageSet.initLinearImages(m_vkDevCtx,
                                        pDecodeProfile,
                                        numImages,
                                        dpbImageFormat,
                                        maxImageExtent,
                                        dpbImageUsage,
                                        queueFamilyIndex,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                        useImageArray, useImageViewArray);
        }

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
        m_perFrameDecodeImageSet[picId].m_frameUsesFg = pDecodePictureInfo->filmGrainEnabled;
        if (m_debug) {
            std::cout << "==> Queue Decode Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                      << std::endl;
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
            pDecodedFrame->outputImageView = m_perFrameDecodeImageSet[pictureIndex].GetDisplayImageView();
            pDecodedFrame->outLinearImage = m_perFrameDecodeImageSet[pictureIndex].GetLinearImage();
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

    virtual int32_t AcquireImageResourceArrayByIndex(uint32_t numResources,
                                                     const int8_t* referenceSlotIndexes,
                                                     VkVideoPictureResourceInfoKHR* dpbPictureResources,
                                                     PictureResourceInfo* dpbPictureResourcesInfo,
                                                     VkImageLayout newDpbImageLayerLayout)
    {
        assert(dpbPictureResources);
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)referenceSlotIndexes[resId] < m_perFrameDecodeImageSet.size()) {
                VkResult result = m_perFrameDecodeImageSet.GetImage(
                    m_vkDevCtx,
                    referenceSlotIndexes[resId],
                    &dpbPictureResources[resId],
                    &dpbPictureResourcesInfo[resId],
                    ReferenceableImage);

                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    return -1;
                }

                m_perFrameDecodeImageSet.SetNewLayout(m_vkDevCtx, referenceSlotIndexes[resId], newDpbImageLayerLayout, ReferenceableImage);
                assert(dpbPictureResources[resId].sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
                dpbPictureResources[resId].codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                dpbPictureResources[resId].codedExtent = m_codedExtent;
            }
        }
        return numResources;
    }

    virtual int32_t AcquireImageResourceByIndex(int8_t referenceSlotIndex,
                                                VkVideoPictureResourceInfoKHR* pPictureResource,
                                                PictureResourceInfo* pPictureResourceInfo,
                                                VkImageLayout newImageLayerLayout,
                                                ImageType imageType)
    {
        assert(pPictureResource);
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)referenceSlotIndex < m_perFrameDecodeImageSet.size()) {
            VkResult result = m_perFrameDecodeImageSet.GetImage(
                m_vkDevCtx,
                referenceSlotIndex,
                pPictureResource,
                pPictureResourceInfo,
                imageType);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }

            m_perFrameDecodeImageSet.SetNewLayout(
                m_vkDevCtx,
                referenceSlotIndex,
                newImageLayerLayout,
                imageType);

            assert(pPictureResource->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
            pPictureResource->codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
            pPictureResource->codedExtent = m_codedExtent;
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

VkResult NvPerFrameDecodeResources::CreateImage(const VulkanDeviceContext* vkDevCtx,
                                                const VkImageCreateInfo* pImageCreateInfo,
                                                VkMemoryPropertyFlags    requiredMemProps,
                                                uint32_t imageIndex,
                                                VkSharedBaseObj<VkImageResource>& imageArrayParent,
                                                VkSharedBaseObj<VkImageResourceView>& imageViewArrayParent,
                                                ImageType imageType)
{
    VkResult result = VK_SUCCESS;
    if ((ImageExist(imageType) != false) && (m_recreateImage[imageType] == false)) {
        m_currentDpbImageLayerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_currentOutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_recreateImage[imageType] = false;
        return result;
    }

    assert(m_vkDevCtx != nullptr);

    // TODO: Decide on a better way to merge the 3 different ImageType options here.
    // 
    if (imageType == ReferenceableImage) {
        m_currentDpbImageLayerLayout = pImageCreateInfo->initialLayout;
        VkSharedBaseObj<VkImageResource> imageResource;
        if (!imageArrayParent) {
            result = VkImageResource::Create(vkDevCtx,
                                             pImageCreateInfo,
                                             requiredMemProps,
                                             imageResource);
            if (result != VK_SUCCESS) {
                return result;
            }
        } else {
            // Use a passed in image instead.
            imageResource = imageArrayParent;
        }

        if (imageViewArrayParent) {
            m_frameDpbImageView = imageViewArrayParent;
            // If no presentable images are created, views need to be created here.
            // But there is no need to create these incase 
        } else {
            uint32_t baseArrayLayer = imageArrayParent ? imageIndex : 0;
            VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseArrayLayer, 1 };
            result = VkImageResourceView::Create(vkDevCtx, imageResource, subresourceRange, m_frameDpbImageView);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
    }

    if (imageType == PresentableImage) {
        m_currentOutputImageLayout = pImageCreateInfo->initialLayout;
        VkSharedBaseObj<VkImageResource> imageResource;
        VkImageSubresourceRange subresourceRange;
        if (!imageArrayParent) {
            subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            result = VkImageResource::Create(vkDevCtx,
                                             pImageCreateInfo,
                                             requiredMemProps,
                                             imageResource);
            if (result != VK_SUCCESS) {
                return result;
            }
        } else {
            // Use a passed in image instead.
            imageResource = imageArrayParent;
            subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, imageIndex, 1 };
        }

        if (result != VK_SUCCESS) {
            return result;
        }

        result = VkImageResourceView::Create(vkDevCtx,
                                             imageResource,
                                             subresourceRange,
                                             m_outImageView);

        if (result != VK_SUCCESS) {
            return result;
        }
    }

    if (imageType == HostVisibleImage) {
        VkImageCreateInfo outImageLinearCreateInfo(*pImageCreateInfo);
        outImageLinearCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;

        assert(requiredMemProps == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
        result = VkImageResource::Create(vkDevCtx,
                                         pImageCreateInfo,
                                         requiredMemProps,
                                         m_outLinearImage);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    if (m_recreateImage[imageType] != false) {
        m_recreateImage[imageType] = false;
    }

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

int32_t NvPerFrameDecodeImageSet::initDpbImages(const VulkanDeviceContext* vkDevCtx,
                                       const VkVideoProfileInfoKHR* pDecodeProfile,
                                       uint32_t                 numImages,
                                       VkFormat                 dpbImageFormat,
                                       const VkExtent2D&        maxImageExtent,
                                       VkImageUsageFlags        dpbImageUsage,
                                       uint32_t                 queueFamilyIndex,
                                       VkMemoryPropertyFlags    dpbRequiredMemProps,
                                       bool                     useImageArray,
                                       bool                     useImageViewArray)
{
    if (numImages > m_perFrameDecodeResources.size()) {
        assert(!"Number of requested images exceeds the max size of the image array");
        return -1;
    }

    const bool reconfigureImages = (m_numImages[ReferenceableImage] &&
        (m_dpbImageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_dpbImageCreateInfo.format != dpbImageFormat) ||
               (m_dpbImageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_dpbImageCreateInfo.extent.height < maxImageExtent.height));

    for (uint32_t imageIndex = m_numImages[ReferenceableImage]; imageIndex < numImages; imageIndex++) {
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

    uint32_t firstIndex = reconfigureImages ? 0 : m_numImages[ReferenceableImage];
    uint32_t maxNumImages = std::max(m_numImages[ReferenceableImage], numImages);
    for (uint32_t imageIndex = firstIndex; imageIndex < maxNumImages; imageIndex++) {

        if (m_perFrameDecodeResources[imageIndex].ImageExist(ReferenceableImage) && reconfigureImages) {

            m_perFrameDecodeResources[imageIndex].m_recreateImage[ReferenceableImage] = true;

        } else if (!m_perFrameDecodeResources[imageIndex].ImageExist(ReferenceableImage)) {

            // TODO: Shouldn't CreateImage happen automatically as part of the GetImage code? Why is this needed?
            VkResult result =
                     m_perFrameDecodeResources[imageIndex].CreateImage(vkDevCtx,
                                                                       &m_dpbImageCreateInfo,
                                                                       m_dpbRequiredMemProps,
                                                                       imageIndex,
                                                                       m_imageArray,
                                                                       m_imageViewArray,
                                                                       ReferenceableImage);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }
        }
    }

    m_numImages[ReferenceableImage] = numImages;
    m_usesImageArray              = useImageArray;
    m_usesImageViewArray          = useImageViewArray;

    return (int32_t)numImages;
}

int32_t NvPerFrameDecodeImageSet::initOutputImages(const VulkanDeviceContext* vkDevCtx,
                                       const VkVideoProfileInfoKHR*,
                                       uint32_t                 numImages,
                                       VkFormat                 outImageFormat,
                                       const VkExtent2D&        maxImageExtent,
                                       VkImageUsageFlags        outImageUsage,
                                       uint32_t                 ,
                                       VkMemoryPropertyFlags    outRequiredMemProps,
                                       bool                     useImageArray,
                                       bool                     useImageViewArray)
{
    // Image arrays are not supported for output specific images
    assert(useImageViewArray == false);

    const bool reconfigureImages = (m_numImages[PresentableImage] &&
        (m_outImageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_outImageCreateInfo.format != outImageFormat) ||
               (m_outImageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_outImageCreateInfo.extent.height < maxImageExtent.height));

    // Image create info for the output
    m_outRequiredMemProps = outRequiredMemProps;
    // Image create info for the DPBs
    m_outImageCreateInfo = m_dpbImageCreateInfo;
    m_outImageCreateInfo.format = outImageFormat;
    m_outImageCreateInfo.arrayLayers = 1;
    m_outImageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    m_outImageCreateInfo.usage = outImageUsage;

    if ((outImageUsage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0) {
        // A simple output image not directly used by the decoder
        m_outImageCreateInfo.pNext = nullptr;
    }

    uint32_t firstIndex = reconfigureImages ? 0 : m_numImages[PresentableImage];
    uint32_t maxNumImages = std::max(m_numImages[PresentableImage], numImages);
    for (uint32_t imageIndex = firstIndex; imageIndex < maxNumImages; imageIndex++) {

        if (m_perFrameDecodeResources[imageIndex].ImageExist(PresentableImage) && reconfigureImages) {

            m_perFrameDecodeResources[imageIndex].m_recreateImage[PresentableImage] = true;

        } else if (!m_perFrameDecodeResources[imageIndex].ImageExist(PresentableImage)) {
            VkSharedBaseObj<VkImageResource> imageArray;
            if (useImageArray != false) {
                 imageArray = m_imageArray;
            }

            // Shouldn't CreateImage happen automatically as part of the GetImage code? Why is this needed?
            VkResult result =
                     m_perFrameDecodeResources[imageIndex].CreateImage(vkDevCtx,
                                                                       &m_outImageCreateInfo,
                                                                       outRequiredMemProps,
                                                                       imageIndex,
                                                                       imageArray,
                                                                       m_imageViewArray,
                                                                       PresentableImage);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
    }

    m_numImages[PresentableImage] = numImages;

    return VK_SUCCESS;
}

int32_t NvPerFrameDecodeImageSet::initLinearImages(const VulkanDeviceContext* vkDevCtx,
                                       const VkVideoProfileInfoKHR*,
                                       uint32_t                 numImages,
                                       VkFormat                 imageFormat,
                                       const VkExtent2D&        maxImageExtent,
                                       VkImageUsageFlags        imageUsage,
                                       uint32_t                 ,
                                       VkMemoryPropertyFlags    requiredMemProps,
                                       bool                     useImageArray,
                                       bool                     useImageViewArray)
{
    // ImageArrays and ViewArrays are unsupported for linearImages, however they could be.
    assert(useImageArray == false);
    assert(useImageViewArray == false);

    const bool reconfigureImages = (m_numImages[HostVisibleImage] &&
        (m_linearOutImageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_linearOutImageCreateInfo.format != imageFormat) ||
               (m_linearOutImageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_linearOutImageCreateInfo.extent.height < maxImageExtent.height));

    m_linearRequiredMemProps = requiredMemProps;

    // When an output image is required and SeparateOutputImage is already used for AV1 filmgrain,
    // create a 3rd separate image to do a linear copy to.
    m_linearOutImageCreateInfo = m_dpbImageCreateInfo;
    m_linearOutImageCreateInfo.format = imageFormat;
    m_linearOutImageCreateInfo.arrayLayers = 1;
    m_linearOutImageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
    m_linearOutImageCreateInfo.usage = imageUsage;

    assert(imageUsage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));

    uint32_t firstIndex = reconfigureImages ? 0 : m_numImages[HostVisibleImage];
    uint32_t maxNumImages = std::max(m_numImages[HostVisibleImage], numImages);
    for (uint32_t imageIndex = firstIndex; imageIndex < maxNumImages; imageIndex++) {

        if (m_perFrameDecodeResources[imageIndex].ImageExist(HostVisibleImage) && reconfigureImages) {
            m_perFrameDecodeResources[imageIndex].m_recreateImage[HostVisibleImage] = true;

        } else if (!m_perFrameDecodeResources[imageIndex].ImageExist(HostVisibleImage)) {

            VkResult result =
                     m_perFrameDecodeResources[imageIndex].CreateImage(vkDevCtx,
                                                                       &m_linearOutImageCreateInfo,
                                                                       requiredMemProps,
                                                                       imageIndex,
                                                                       m_imageArray,
                                                                       m_imageViewArray,
                                                                       HostVisibleImage);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
    }

    m_numImages[HostVisibleImage] = numImages;
    m_linearOutImageCreateInfo.pNext = nullptr;
    return VK_SUCCESS;
}

void NvPerFrameDecodeImageSet::Deinit()
{
    for (size_t ndx = 0; ndx < m_perFrameDecodeResources.size(); ndx++) {
        m_perFrameDecodeResources[ndx].Deinit();
    }
}
