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
#include <queue>
#include <sstream>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#include "PictureBufferBase.h"
#include "VkCodecUtils/HelpersDispatchTable.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VulkanVideoFrameBuffer.h"
#include "vk_enum_string_helper.h"
#include "vulkan_interfaces.h"

#define MAX_FRAMEBUFFER_IMAGES 32

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#define CLOCK_MONOTONIC 0
extern int clock_gettime(int dummy, struct timespec* ct);
#endif

class NvPerFrameDecodeImage : public vkPicBuffBase {
public:
    NvPerFrameDecodeImage()
        : m_picDispInfo()
        , m_frameImage()
        , m_currentImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
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
    {
    }

    void Deinit();

    ~NvPerFrameDecodeImage()
    {
        Deinit();
    }

    VkParserDecodePictureInfo m_picDispInfo;
    vulkanVideoUtils::ImageObject m_frameImage;
    VkImageLayout m_currentImageLayout;
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
};

class NvPerFrameDecodeImageSet {
public:
    NvPerFrameDecodeImageSet()
        : m_size(0)
        , m_frameDecodeImages()
    {
    }

    int32_t init(uint32_t numImages,
        vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
        const VkImageCreateInfo* pImageCreateInfo,
        VkMemoryPropertyFlags requiredMemProps = 0,
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
        assert(index < m_size);
        return m_frameDecodeImages[index];
    }

    size_t size()
    {
        return m_size;
    }

private:
    size_t m_size;
    NvPerFrameDecodeImage m_frameDecodeImages[MAX_FRAMEBUFFER_IMAGES];
};

static uint64_t getNsTime(bool resetTime = false)
{
    static bool initStart = false;
    static struct timespec start_;
    if (!initStart || resetTime) {
        clock_gettime(CLOCK_MONOTONIC, &start_);
        initStart = true;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    constexpr long one_sec_in_ns = 1000 * 1000 * 1000;

    time_t secs = now.tv_sec - start_.tv_sec;
    uint64_t nsec;
    if (now.tv_nsec > start_.tv_nsec) {
        nsec = now.tv_nsec - start_.tv_nsec;
    } else {
        if(secs > 1) {
            secs--;
        } else if (secs < 0) {
            secs = 0;
        }
        nsec = one_sec_in_ns - (start_.tv_nsec - now.tv_nsec);
    }

    return (secs * one_sec_in_ns) + nsec;
}

class NvVulkanVideoFrameBuffer : public VulkanVideoFrameBuffer {
public:
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
        , m_extent { 0, 0 }
        , m_debug()
    {
    }

    virtual int32_t AddRef();
    virtual int32_t Release();

    VkResult CreateVideoQueries(uint32_t numSlots, vulkanVideoUtils::VulkanDeviceInfo* deviceInfo, const VkVideoProfileKHR* pDecodeProfile)
    {
        VkPhysicalDeviceFeatures2 coreFeatures;
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures;

        memset(&coreFeatures, 0, sizeof(coreFeatures));
        memset(&ycbcrFeatures, 0, sizeof(ycbcrFeatures));

        coreFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        coreFeatures.pNext = &ycbcrFeatures;
        ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;

        vk::GetPhysicalDeviceFeatures2(deviceInfo->physDevice_, &coreFeatures);

        VkQueryPoolCreateInfo queryPoolCreateInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryPoolCreateInfo.pNext = pDecodeProfile;
        queryPoolCreateInfo.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
        queryPoolCreateInfo.queryCount = numSlots; // m_numDecodeSurfaces frames worth

        return vk::CreateQueryPool(deviceInfo->device_, &queryPoolCreateInfo, NULL, &m_queryPool);
    }

    virtual int32_t InitImagePool(uint32_t numImages, const VkImageCreateInfo* pImageCreateInfo, const VkVideoProfileKHR* pDecodeProfile = NULL)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);

        while (!m_displayFrames.empty()) {
            int8_t pictureIndex = m_displayFrames.front();
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameDecodeImageSet.size()));
            m_displayFrames.pop();
            assert(m_perFrameDecodeImageSet[(uint32_t)pictureIndex].IsAvailable());
            m_perFrameDecodeImageSet[(uint32_t)pictureIndex].Release();
        }

        if (m_queryPool != VkQueryPool()) {
            vk::DestroyQueryPool(m_pVideoRendererDeviceInfo->device_, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }

        m_ownedByDisplayMask = 0;
        m_frameNumInDecodeOrder = 0;
        m_frameNumInDisplayOrder = 0;

        if (numImages && pDecodeProfile) {
            VkResult result = CreateVideoQueries(numImages, m_pVideoRendererDeviceInfo, pDecodeProfile);
            if (result != VK_SUCCESS) {
                return 0;
            }
        }

        if (numImages && pImageCreateInfo) {

            m_extent.width = pImageCreateInfo->extent.width;
            m_extent.height = pImageCreateInfo->extent.height;

            return m_perFrameDecodeImageSet.init(numImages, m_pVideoRendererDeviceInfo, pImageCreateInfo,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                0 /* No ColorPatternColorBars */,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);

        } else {
            m_perFrameDecodeImageSet.Deinit();
        }

        return 0;
    }

    virtual int32_t QueueDecodedPictureForDisplay(int8_t picId, VulkanVideoDisplayPictureInfo* pDispInfo)
    {
        assert((uint32_t)picId < m_perFrameDecodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameDecodeImageSet[picId].m_displayOrder = m_frameNumInDisplayOrder++;
        m_perFrameDecodeImageSet[picId].m_timestamp = pDispInfo->timestamp;
        m_perFrameDecodeImageSet[picId].m_inDisplayQueue = true;
        m_perFrameDecodeImageSet[picId].AddRef();

        m_displayFrames.push((uint8_t)picId);

        std::cout << "==> Queue Display Picture picIdx: " << (uint32_t)picId
                  << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                  << "\ttimestamp " << m_perFrameDecodeImageSet[picId].m_timestamp << std::endl;

        return picId;
    }

    virtual int32_t QueuePictureForDecode(int8_t picId, VkParserDecodePictureInfo* pDecodePictureInfo, FrameSynchronizationInfo* pFrameSynchronizationInfo)
    {
        assert((uint32_t)picId < m_perFrameDecodeImageSet.size());

        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        m_perFrameDecodeImageSet[picId].m_picDispInfo = *pDecodePictureInfo;
        m_perFrameDecodeImageSet[picId].m_decodeOrder = m_frameNumInDecodeOrder++;
        m_perFrameDecodeImageSet[picId].m_inDecodeQueue = true;
        std::cout << "==> Queue Decode Picture picIdx: " << (uint32_t)picId
                  << "\t\tdisplayOrder: " << m_perFrameDecodeImageSet[picId].m_displayOrder << "\tdecodeOrder: " << m_perFrameDecodeImageSet[picId].m_decodeOrder
                  << "\ttimestamp " << getNsTime() << "\tFrameType " << m_perFrameDecodeImageSet[picId].m_picDispInfo.videoFrameType << std::endl;

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

            pDecodedFrame->pDecodedImage = &m_perFrameDecodeImageSet[pictureIndex].m_frameImage;

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
            int pictureIndex = pDecodedFrameRelease->pictureIndex;
            assert((pictureIndex >= 0) && ((uint32_t)pictureIndex < m_perFrameDecodeImageSet.size()));

            assert(m_perFrameDecodeImageSet[pictureIndex].m_decodeOrder == pDecodedFrameRelease->decodeOrder);
            assert(m_perFrameDecodeImageSet[pictureIndex].m_displayOrder == pDecodedFrameRelease->displayOrder);

            assert(m_ownedByDisplayMask & (1 << pictureIndex));
            m_ownedByDisplayMask &= ~(1 << pictureIndex);
            m_perFrameDecodeImageSet[pictureIndex].m_ownedByDisplay = false;
            m_perFrameDecodeImageSet[pictureIndex].Release();

            m_perFrameDecodeImageSet[pictureIndex].m_hasConsummerSignalFence = pDecodedFrameRelease->hasConsummerSignalFence;
            m_perFrameDecodeImageSet[pictureIndex].m_hasConsummerSignalSemaphore = pDecodedFrameRelease->hasConsummerSignalSemaphore;
        }
        return 0;
    }

    virtual int32_t GetImageResourcesByIndex(uint32_t numResources, const int8_t* referenceSlotIndexes,
        VkVideoPictureResourceKHR* pictureResources,
        PictureResourceInfo* pictureResourcesInfo,
        VkImageLayout newImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM)
    {
        std::lock_guard<std::mutex> lock(m_displayQueueMutex);
        for (unsigned int resId = 0; resId < numResources; resId++) {
            if ((uint32_t)referenceSlotIndexes[resId] < m_perFrameDecodeImageSet.size()) {
                pictureResources[resId].imageViewBinding = m_perFrameDecodeImageSet[referenceSlotIndexes[resId]].m_frameImage.view;
                assert(pictureResources[resId].sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_KHR);
                pictureResources[resId].codedOffset = { 0, 0 }; // FIXME: This parameter must to be adjusted based on the interlaced mode.
                pictureResources[resId].codedExtent = m_extent;
                pictureResources[resId].baseArrayLayer = 0;
                if (pictureResourcesInfo) {
                    pictureResourcesInfo[resId].currentImageLayout = m_perFrameDecodeImageSet[referenceSlotIndexes[resId]].m_currentImageLayout;
                }
                if (VK_IMAGE_LAYOUT_MAX_ENUM != newImageLayout) {
                    m_perFrameDecodeImageSet[referenceSlotIndexes[resId]].m_currentImageLayout = newImageLayout;
                    if (pictureResourcesInfo) {
                        pictureResourcesInfo[resId].image = m_perFrameDecodeImageSet[referenceSlotIndexes[resId]].m_frameImage.image;
                    }
                }
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
            return &m_perFrameDecodeImageSet[picId].m_frameImage;
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
    void Deinitialize() {};

    virtual ~NvVulkanVideoFrameBuffer()
    {
        if (m_queryPool != VkQueryPool()) {
            vk::DestroyQueryPool(m_pVideoRendererDeviceInfo->device_, m_queryPool, NULL);
            m_queryPool = VkQueryPool();
        }
    }

    struct PpsEntry {

    };

    struct SpsEntry {
        std::map<uint8_t, PpsEntry> ppsMap;
    };

private:
    vulkanVideoUtils::VulkanDeviceInfo* m_pVideoRendererDeviceInfo;
    std::atomic<int32_t> m_refCount;
    std::mutex m_displayQueueMutex;
    NvPerFrameDecodeImageSet m_perFrameDecodeImageSet;
    std::queue<uint8_t> m_displayFrames;
    VkQueryPool m_queryPool;
    uint32_t m_ownedByDisplayMask;
    int32_t m_frameNumInDecodeOrder;
    int32_t m_frameNumInDisplayOrder;
    VkExtent2D m_extent;
    uint32_t m_debug : 1;
    std::map<uint8_t, SpsEntry> spsMap;
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

void NvPerFrameDecodeImage::Deinit()
{
    if (m_frameImage.m_device == VkDevice()) {
        return;
    }

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

    m_frameImage.DestroyImage();
    Reset();
}

int32_t NvPerFrameDecodeImageSet::init(uint32_t numImages,
    vulkanVideoUtils::VulkanDeviceInfo* deviceInfo,
    const VkImageCreateInfo* pImageCreateInfo,
    VkMemoryPropertyFlags requiredMemProps,
    int initWithPattern,
    VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes,
    vulkanVideoUtils::NativeHandle& importHandle)
{
    Deinit();

    m_size = numImages;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFenceCreateInfo fenceFrameCompleteInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    // The fence waited on for the first frame should be signaled.
    fenceFrameCompleteInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    for (unsigned imageIndex = 0; imageIndex < numImages; imageIndex++) {
        VkResult result = m_frameDecodeImages[imageIndex].m_frameImage.CreateImage(deviceInfo, pImageCreateInfo,
            requiredMemProps,
            initWithPattern,
            exportMemHandleTypes, importHandle);
        assert(result == VK_SUCCESS);
        result = vk::CreateFence(deviceInfo->device_, &fenceFrameCompleteInfo, nullptr, &m_frameDecodeImages[imageIndex].m_frameCompleteFence);
        result = vk::CreateFence(deviceInfo->device_, &fenceInfo, nullptr, &m_frameDecodeImages[imageIndex].m_frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
        assert(result == VK_SUCCESS);
        result = vk::CreateSemaphore(deviceInfo->device_, &semInfo, nullptr, &m_frameDecodeImages[imageIndex].m_frameCompleteSemaphore);
        assert(result == VK_SUCCESS);
        result = vk::CreateSemaphore(deviceInfo->device_, &semInfo, nullptr, &m_frameDecodeImages[imageIndex].m_frameConsumerDoneSemaphore);
        assert(result == VK_SUCCESS);
    }

    return (int32_t)m_size;
}

void NvPerFrameDecodeImageSet::Deinit()
{
    for (uint32_t ndx = 0; ndx < m_size; ndx++) {
        m_frameDecodeImages[ndx].Deinit();
    }
    m_size = 0;
}
