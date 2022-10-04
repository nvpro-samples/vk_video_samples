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

#include "PictureBufferBase.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VkCodecUtils/nvVideoProfile.h"
#include "VulkanVideoFrameBuffer.h"
#include "vk_enum_string_helper.h"
#include "vulkan_interfaces.h"

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#define CLOCK_MONOTONIC 0
extern int clock_gettime(int dummy, struct timespec* ct);
#else
#include <time.h>
#endif

class NvPerFrameDecodeImage : public vkPicBuffBase {
public:
    NvPerFrameDecodeImage()
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
        , m_frameImage()
        , m_currentImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
    }

    VkResult CreateImage( vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
                          const VkImageCreateInfo* pImageCreateInfo,
                          VkMemoryPropertyFlags requiredMemProps);

    VkResult init( vulkanVideoUtils::VulkanDeviceInfo* deviceInfo);

    void Deinit();

    NvPerFrameDecodeImage (const NvPerFrameDecodeImage &srcObj) = delete;
    NvPerFrameDecodeImage (NvPerFrameDecodeImage &&srcObj) = delete;

    ~NvPerFrameDecodeImage()
    {
        Deinit();
    }

    const vulkanVideoUtils::ImageObject* GetImageObject() {
        if (ImageExist()) {
            return &m_frameImage;
        } else {
            return nullptr;
        }
    }

    bool ImageExist() {

        return !!m_frameImage;
    }

    bool GetImageSetNewLayout(VkImageLayout newImageLayout,
                              VkImage* pImage,
                              VkImageView* pImageView,
                              VkImageLayout* pOldImageLayout = nullptr) {


        if (m_recreateImage || !ImageExist()) {
            return false;
        }

        if (pImage) {
            *pImage = m_frameImage.image;
        }

        if (pImageView) {
            *pImageView = m_frameImage.view;
        }

        if (pOldImageLayout) {
            *pOldImageLayout = m_currentImageLayout;
        }

        if (VK_IMAGE_LAYOUT_MAX_ENUM != newImageLayout) {
            m_currentImageLayout = newImageLayout;
        }

        return true;
    }

    VkImageLayout ResetImageLayout(VkImageLayout newImageLayout = VK_IMAGE_LAYOUT_UNDEFINED) {
        VkImageLayout oldImageLayout = m_currentImageLayout;
        m_currentImageLayout =  newImageLayout;
        return oldImageLayout;
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
    VkSharedBaseObj<VkParserVideoRefCountBase> currentVkPictureParameters;

private:
    vulkanVideoUtils::ImageObject m_frameImage;
    VkImageLayout                 m_currentImageLayout;
};

class NvPerFrameDecodeImageSet {
public:

    static constexpr size_t maxImages = 32;

    NvPerFrameDecodeImageSet()
        : m_queueFamilyIndex((uint32_t)-1),
          m_imageCreateInfo(),
          m_requiredMemProps(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
          m_numImages(0),
          m_frameDecodeImages(maxImages)
    {
    }

    int32_t init(vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
        const VkVideoProfileInfoKHR* pDecodeProfile,
        uint32_t              numImages,
        VkFormat              imageFormat,
        const VkExtent2D&     maxImageExtent,
        VkImageTiling         tiling,
        VkImageUsageFlags     usage,
        uint32_t              queueFamilyIndex,
        VkMemoryPropertyFlags requiredMemProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        int initWithPattern = -1,
        VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes = VkExternalMemoryHandleTypeFlagBitsKHR(),
        vulkanVideoUtils::NativeHandle& importHandle = vulkanVideoUtils::NativeHandle::InvalidNativeHandle);

    void Deinit();

    ~NvPerFrameDecodeImageSet()
    {
        Deinit();
    }

    NvPerFrameDecodeImage& operator[](unsigned int index)
    {
        assert(index < m_frameDecodeImages.size());
        return m_frameDecodeImages[index];
    }

    size_t size()
    {
        return m_numImages;
    }


    VkResult GetImageSetNewLayout(vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo,
                                  uint32_t imageIndex,
                                  VkImageLayout newImageLayout,
                                  VkImage* pImage,
                                  VkImageView* pImageView,
                                  VkImageLayout* pOldImageLayout = nullptr) {

        VkResult result = VK_SUCCESS;
        bool validImage = m_frameDecodeImages[imageIndex].GetImageSetNewLayout(
                               newImageLayout,
                               pImage,
                               pImageView,
                               pOldImageLayout);

        if (!validImage) {
            result = m_frameDecodeImages[imageIndex].CreateImage(
                               pVideoRendererDeviceInfo,
                               &m_imageCreateInfo,
                               m_requiredMemProps);

            if (result == VK_SUCCESS) {
                validImage = m_frameDecodeImages[imageIndex].GetImageSetNewLayout(
                                               newImageLayout,
                                               pImage,
                                               pImageView,
                                               pOldImageLayout);

                assert(validImage);
            }
        }

        return result;
    }

private:
    uint32_t                            m_queueFamilyIndex;
    nvVideoProfile                      m_videoProfile;
    VkImageCreateInfo                   m_imageCreateInfo;
    VkMemoryPropertyFlags               m_requiredMemProps;
    uint32_t                            m_numImages;
    std::vector<NvPerFrameDecodeImage>  m_frameDecodeImages;
};

class NvVulkanVideoFrameBuffer : public VulkanVideoFrameBuffer {
public:

    static constexpr size_t maxFramebufferImages = 32;

    NvVulkanVideoFrameBuffer(vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo)
        : m_pVideoRendererDeviceInfo(pVideoRendererDeviceInfo)
        , m_refCount(1)
        , m_displayQueueMutex()
        , m_perFrameDecodeImageSet()
        , m_displayFrames()
        , m_queryPool()
        , m_ownedByDisplayMask(0)
        , m_frameNumInDecodeOrder(0)
        , m_frameNumInDisplayOrder(0)
        , m_codedExtent { 0, 0 }
        , m_numberParameterUpdates(0)
        , m_debug()
    {
    }

    virtual int32_t AddRef();
    virtual int32_t Release();

    VkResult CreateVideoQueries(uint32_t numSlots, vulkanVideoUtils::VulkanDeviceInfo* deviceInfo, const VkVideoProfileInfoKHR* pDecodeProfile)
    {
        assert (numSlots <= maxFramebufferImages);

        if (m_queryPool == VkQueryPool()) {
            // It would be difficult to resize a query pool, so allocate the maximum possible slot.
            numSlots = maxFramebufferImages;
            VkQueryPoolCreateInfo queryPoolCreateInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryPoolCreateInfo.pNext = pDecodeProfile;
            queryPoolCreateInfo.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
            queryPoolCreateInfo.queryCount = numSlots; // m_numDecodeSurfaces frames worth

            return vk::CreateQueryPool(deviceInfo->device_, &queryPoolCreateInfo, NULL, &m_queryPool);
        }

        return VK_SUCCESS;
    }

    void DestroyVideoQueries() {
        if (m_queryPool != VkQueryPool()) {
            vk::DestroyQueryPool(m_pVideoRendererDeviceInfo->device_, m_queryPool, NULL);
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
            assert(m_perFrameDecodeImageSet[(uint32_t)pictureIndex].IsAvailable());
            m_perFrameDecodeImageSet[(uint32_t)pictureIndex].Release();
            flushedImages++;
        }

        return flushedImages;
    }

    virtual int32_t InitImagePool(const VkVideoProfileInfoKHR* pDecodeProfile,
                                  uint32_t                 numImages,
                                  VkFormat                 imageFormat,
                                  const VkExtent2D&        codedExtent,
                                  const VkExtent2D&        maxImageExtent,
                                  VkImageTiling            tiling,
                                  VkImageUsageFlags        usage,
                                  uint32_t                 queueFamilyIndex)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        assert(numImages && (numImages <= maxFramebufferImages) && pDecodeProfile);

        VkResult result = CreateVideoQueries(numImages, m_pVideoRendererDeviceInfo, pDecodeProfile);
        if (result != VK_SUCCESS) {
            return 0;
        }

        // m_extent is for the codedExtent, not the max image resolution
        m_codedExtent = codedExtent;

        int32_t imageSetCreateResult =
                m_perFrameDecodeImageSet.init(m_pVideoRendererDeviceInfo,
                                              pDecodeProfile,
                                              numImages,
                                              imageFormat,
                                              maxImageExtent,
                                              tiling,
                                              usage,
                                              queueFamilyIndex,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              0 /* No ColorPatternColorBars */,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                              vulkanVideoUtils::NativeHandle::InvalidNativeHandle);
        m_numberParameterUpdates++;

        return imageSetCreateResult;
    }

    void Deinitialize() {

        FlushDisplayQueue();

        DestroyVideoQueries();

        m_ownedByDisplayMask = 0;
        m_frameNumInDecodeOrder = 0;
        m_frameNumInDisplayOrder = 0;

        m_perFrameDecodeImageSet.Deinit();
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
                                          VkParserVideoRefCountBase* pCurrentVkPictureParameters,
                                          FrameSynchronizationInfo* pFrameSynchronizationInfo)
    {
        assert((uint32_t)picId < m_perFrameDecodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameDecodeImageSet[picId].m_picDispInfo = *pDecodePictureInfo;
        m_perFrameDecodeImageSet[picId].m_decodeOrder = m_frameNumInDecodeOrder++;
        m_perFrameDecodeImageSet[picId].m_inDecodeQueue = true;
        m_perFrameDecodeImageSet[picId].currentVkPictureParameters = pCurrentVkPictureParameters;

        if (m_debug) {
            std::cout << "==> Queue Decode Picture picIdx: " << (uint32_t)picId
                      << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                      << "\ttimestamp " << vulkanVideoUtils::getNsTime() << "\tFrameType " << m_perFrameDecodeImageSet[picId].m_picDispInfo.videoFrameType << std::endl;
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

            pDecodedFrame->pDecodedImage = m_perFrameDecodeImageSet[pictureIndex].GetImageObject();

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
            m_perFrameDecodeImageSet[picId].currentVkPictureParameters = nullptr;
            m_perFrameDecodeImageSet[picId].m_ownedByDisplay = false;
            m_perFrameDecodeImageSet[picId].Release();

            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalFence = pDecodedFrameRelease->hasConsummerSignalFence;
            m_perFrameDecodeImageSet[picId].m_hasConsummerSignalSemaphore = pDecodedFrameRelease->hasConsummerSignalSemaphore;
        }
        return 0;
    }

    virtual int32_t GetImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes,
        VkVideoPictureResourceInfoKHR* pictureResources,
        PictureResourceInfo* pictureResourcesInfo,
        VkImageLayout newImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)referenceSlotIndexes[resId] < m_perFrameDecodeImageSet.size()) {

                VkResult result = m_perFrameDecodeImageSet.GetImageSetNewLayout(m_pVideoRendererDeviceInfo,
                                     referenceSlotIndexes[resId],
                                     newImageLayout,
                                     pictureResourcesInfo ? &pictureResourcesInfo[resId].image : nullptr,
                                     &pictureResources[resId].imageViewBinding,
                                     pictureResourcesInfo ?  &pictureResourcesInfo[resId].currentImageLayout : nullptr);

                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    return -1;
                }

                assert(pictureResources[resId].sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
                pictureResources[resId].codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                pictureResources[resId].codedExtent = m_codedExtent;
                pictureResources[resId].baseArrayLayer = 0;
            }
        }
        return numResources;
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

    virtual int32_t SetPicNumInDecodeOrder(int32_t picId, int32_t picNumInDecodeOrder)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameDecodeImageSet.size()) {
            int32_t oldPicNumInDecodeOrder = m_perFrameDecodeImageSet[picId].m_decodeOrder;
            m_perFrameDecodeImageSet[picId].m_decodeOrder = picNumInDecodeOrder;
            return oldPicNumInDecodeOrder;
        }
        assert(false);
        return -1;
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

    virtual const vulkanVideoUtils::ImageObject* GetImageResourceByIndex(int8_t picId)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        if ((uint32_t)picId < m_perFrameDecodeImageSet.size()) {
            return m_perFrameDecodeImageSet[picId].GetImageObject();
        }
        assert(false);
        return NULL;
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

    VkResult Initialize() { return VK_SUCCESS; }

    virtual ~NvVulkanVideoFrameBuffer()
    {
        if (m_queryPool != VkQueryPool()) {
            vk::DestroyQueryPool(m_pVideoRendererDeviceInfo->device_, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }
    }

private:
    vulkanVideoUtils::VulkanDeviceInfo* m_pVideoRendererDeviceInfo;
    std::atomic<int32_t>     m_refCount;
    std::mutex               m_displayQueueMutex;
    NvPerFrameDecodeImageSet m_perFrameDecodeImageSet;
    std::queue<uint8_t>      m_displayFrames;
    VkQueryPool              m_queryPool;
    uint32_t                 m_ownedByDisplayMask;
    int32_t                  m_frameNumInDecodeOrder;
    int32_t                  m_frameNumInDisplayOrder;
    VkExtent2D               m_codedExtent;               // for the codedExtent, not the max image resolution
    uint32_t                 m_numberParameterUpdates;
    uint32_t                 m_debug : 1;
};

VulkanVideoFrameBuffer* VulkanVideoFrameBuffer::CreateInstance(vulkanVideoUtils::VulkanDeviceInfo* pVideoRendererDeviceInfo)
{
    NvVulkanVideoFrameBuffer* pVulkanVideoFrameBuffer = new NvVulkanVideoFrameBuffer(pVideoRendererDeviceInfo);
    if (!pVulkanVideoFrameBuffer) {
        return pVulkanVideoFrameBuffer;
    }
    VkResult err = pVulkanVideoFrameBuffer->Initialize();
    if (err != VK_SUCCESS) {
        pVulkanVideoFrameBuffer->Release();
        pVulkanVideoFrameBuffer = NULL;
    }
    return pVulkanVideoFrameBuffer;
}

int32_t NvVulkanVideoFrameBuffer::AddRef()
{
    return ++m_refCount;
}

int32_t NvVulkanVideoFrameBuffer::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        Deinitialize();
        delete this;
    }
    return ret;
}

VkResult NvPerFrameDecodeImage::CreateImage( vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
                                             const VkImageCreateInfo* pImageCreateInfo,
                                             VkMemoryPropertyFlags requiredMemProps)
{
    VkResult result = VK_SUCCESS;

    if (!m_frameImage || m_recreateImage) {

        result = m_frameImage.CreateImage(deviceInfo, pImageCreateInfo,
                                          requiredMemProps);

        if (result == VK_SUCCESS) {
            m_currentImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            m_recreateImage = false;
        }
    }

    return result;
}

VkResult NvPerFrameDecodeImage::init( vulkanVideoUtils::VulkanDeviceInfo* deviceInfo)
{

    // The fence waited on for the first frame should be signaled.
    const VkFenceCreateInfo fenceFrameCompleteInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                                       VK_FENCE_CREATE_SIGNALED_BIT };
    VkResult result = vk::CreateFence(deviceInfo->device_, &fenceFrameCompleteInfo, nullptr, &m_frameCompleteFence);

    const VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr };
    result = vk::CreateFence(deviceInfo->device_, &fenceInfo, nullptr, &m_frameConsumerDoneFence);
    assert(result == VK_SUCCESS);

    const VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr };
    result = vk::CreateSemaphore(deviceInfo->device_, &semInfo, nullptr, &m_frameCompleteSemaphore);
    assert(result == VK_SUCCESS);
    result = vk::CreateSemaphore(deviceInfo->device_, &semInfo, nullptr, &m_frameConsumerDoneSemaphore);
    assert(result == VK_SUCCESS);

    Reset();

    return result;
}

void NvPerFrameDecodeImage::Deinit()
{
    currentVkPictureParameters = nullptr;

    if (m_frameCompleteFence != VkFence()) {
        vk::DestroyFence(m_frameImage.m_device, m_frameCompleteFence, nullptr);
        m_frameCompleteFence = VkFence();
    }

    if (m_frameConsumerDoneFence != VkFence()) {
        vk::DestroyFence(m_frameImage.m_device, m_frameConsumerDoneFence, nullptr);
        m_frameConsumerDoneFence = VkFence();
    }

    if (m_frameCompleteSemaphore != VkSemaphore()) {
        vk::DestroySemaphore(m_frameImage.m_device, m_frameCompleteSemaphore, nullptr);
        m_frameCompleteSemaphore = VkSemaphore();
    }

    if (m_frameConsumerDoneSemaphore != VkSemaphore()) {
        vk::DestroySemaphore(m_frameImage.m_device, m_frameConsumerDoneSemaphore, nullptr);
        m_frameConsumerDoneSemaphore = VkSemaphore();
    }

    if (m_frameImage) {
        m_frameImage.DestroyImage();
    }
    Reset();
}

int32_t NvPerFrameDecodeImageSet::init(vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
                                       const VkVideoProfileInfoKHR* pDecodeProfile,
                                       uint32_t              numImages,
                                       VkFormat              imageFormat,
                                       const VkExtent2D&     maxImageExtent,
                                       VkImageTiling         tiling,
                                       VkImageUsageFlags     usage,
                                       uint32_t              queueFamilyIndex,
                                       VkMemoryPropertyFlags requiredMemProps,
                                       int initWithPattern,
                                       VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes,
                                       vulkanVideoUtils::NativeHandle& importHandle)
{
    if (numImages > m_frameDecodeImages.size()) {
        assert(!"Number of requested images exceeds the max size of the image array");
        return -1;
    }

    const bool reconfigureImages = (m_numImages &&
        (m_imageCreateInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)) &&
              ((m_imageCreateInfo.format != imageFormat) ||
               (m_imageCreateInfo.extent.width < maxImageExtent.width) ||
               (m_imageCreateInfo.extent.height < maxImageExtent.height));

    m_videoProfile.InitFromProfile(pDecodeProfile);

    m_queueFamilyIndex = queueFamilyIndex;
    m_requiredMemProps = requiredMemProps;
    m_imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    m_imageCreateInfo.pNext = m_videoProfile.GetProfile();
    m_imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    m_imageCreateInfo.format = imageFormat;
    m_imageCreateInfo.extent = { maxImageExtent.width, maxImageExtent.height, 1 };
    m_imageCreateInfo.mipLevels = 1;
    m_imageCreateInfo.arrayLayers = 1;
    m_imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    m_imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    m_imageCreateInfo.usage = usage;
    m_imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    m_imageCreateInfo.queueFamilyIndexCount = 1;
    m_imageCreateInfo.pQueueFamilyIndices = &m_queueFamilyIndex;
    m_imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_imageCreateInfo.flags = 0;

    for (uint32_t imageIndex = m_numImages; imageIndex < numImages; imageIndex++) {
        VkResult result = m_frameDecodeImages[imageIndex].init(deviceInfo);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            return -1;
        }
    }

    uint32_t firstIndex = reconfigureImages ? 0 : m_numImages;
    uint32_t maxNumImages = std::max(m_numImages, numImages);
    for (uint32_t imageIndex = firstIndex; imageIndex < maxNumImages; imageIndex++) {

        if (m_frameDecodeImages[imageIndex].ImageExist() && reconfigureImages) {

            m_frameDecodeImages[imageIndex].m_recreateImage = true;

        } else if (!m_frameDecodeImages[imageIndex].ImageExist()) {

            VkResult result = m_frameDecodeImages[imageIndex].CreateImage(deviceInfo,
                                                                          &m_imageCreateInfo,
                                                                          m_requiredMemProps);

            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                return -1;
            }
        }
    }

    m_numImages = numImages;
    return (int32_t)numImages;
}

void NvPerFrameDecodeImageSet::Deinit()
{
    for (size_t ndx = 0; ndx < m_frameDecodeImages.size(); ndx++) {
        m_frameDecodeImages[ndx].Deinit();
    }
}
