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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(__unix__)
#include <unistd.h>
#endif

#include <vector>
#include <iostream>     // std::cout
#include <sstream>      // std::stringstream

#ifndef __VULKANVIDEOUTILS__
#define __VULKANVIDEOUTILS__

#include <vulkan_interfaces.h>
#include "VkCodecUtils/VkBufferResource.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanShaderCompiler.h"

namespace vulkanVideoUtils {

template <class valueType, class alignmentType>
valueType alignedSize(valueType value, alignmentType alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

struct Vertex {
    float position[2];
    float texCoord[2];
};

struct Vec2 {
    Vec2(float val0, float val1)
        : val{val0, val1} {}
    float val[2];
};

struct Vec4 {
    Vec4(float val0, float val1, float val2, float val3)
        : val{val0, val1, val2, val3} {}
    float val[4];
};

struct TransformPushConstants {
    TransformPushConstants()
        : posMatrix {{1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f},
                     {0.0f, 0.0f, 0.0f, 1.0f}}
          , texMatrix {{1.0f, 0.0f},
                       {0.0f, 1.0f}}
    {
    }
    Vec4 posMatrix[4];
    Vec2 texMatrix[2];
};

#if defined(VK_USE_PLATFORM_XCB_KHR) || defined (VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
#define VK_PLATFORM_IS_UNIX 1
#endif

class NativeHandle {
public:
    static NativeHandle InvalidNativeHandle;

    NativeHandle(void);
    NativeHandle(const NativeHandle& other);
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    NativeHandle(int fd);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    NativeHandle (AHardwareBufferHandle buffer);
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    ~NativeHandle (void);

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    NativeHandle& operator= (int fd);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    NativeHandle& operator= (AHardwareBufferHandle buffer);
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    int getFd(void) const;
    operator int() const { return getFd(); }
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    AHardwareBufferHandle getAndroidHardwareBuffer(void) const;
    operator AHardwareBufferHandle() const { return getAndroidHardwareBuffer(); }
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    VkExternalMemoryHandleTypeFlagBits getExternalMemoryHandleType (void) const
    {
        return m_externalMemoryHandleType;
    }
    void disown(void);
    bool isValid(void) const;
    operator bool() const { return isValid(); }
    // This should only be called on an import error or on handle replacement.
    void releaseReference(void);

private:
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    int                                 m_fd;
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    AHardwareBufferHandle               m_androidHardwareBuffer;
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    VkExternalMemoryHandleTypeFlagBits  m_externalMemoryHandleType;

    // Disabled
    NativeHandle& operator= (const NativeHandle&) = delete;
};

struct ImageResourceInfo {
    VkFormat       imageFormat;
    int32_t        imageWidth;
    int32_t        imageHeight;
    uint32_t       arrayLayer;
    VkImageLayout  imageLayout;
    VkImage        image;
    VkImageView    view;

    ImageResourceInfo() : imageFormat(VK_FORMAT_UNDEFINED),
                          imageWidth(0),
                          imageHeight(0),
                          arrayLayer(0),
                          imageLayout(VK_IMAGE_LAYOUT_UNDEFINED),
                          image(),
                          view()
    {}

    ImageResourceInfo(VkImageResourceView* pView, VkImageLayout layout) {
        if (pView) {
            VkImageResource* pImage = pView->GetImageResource();
            imageFormat = pImage->GetImageCreateInfo().format;
            imageWidth = pImage->GetImageCreateInfo().extent.width;
            imageHeight = pImage->GetImageCreateInfo().extent.height;
            arrayLayer = pView->GetImageSubresourceRange().baseArrayLayer;
            imageLayout = layout;
            image = pImage->GetImage();
            view = pView->GetImageView();
        } else {
            imageFormat = VK_FORMAT_UNDEFINED;
            imageWidth = 0;
            imageHeight = 0;
            arrayLayer = 0;
            imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image = VK_NULL_HANDLE;
            view = VK_NULL_HANDLE;
        }
    }
};

class VulkanDisplayTiming  {
public:
    VulkanDisplayTiming(const VulkanDeviceContext* vkDevCtx)
    : vkGetRefreshCycleDurationGOOGLE(nullptr),
      vkGetPastPresentationTimingGOOGLE(nullptr)
    {
#ifdef VK_GOOGLE_display_timing
    vkGetRefreshCycleDurationGOOGLE =
            reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
                    vkDevCtx->GetDeviceProcAddr(vkDevCtx->getDevice(), "vkGetRefreshCycleDurationGOOGLE"));
    vkGetPastPresentationTimingGOOGLE =
            reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                    vkDevCtx->GetDeviceProcAddr(vkDevCtx->getDevice(), "vkGetPastPresentationTimingGOOGLE"));

#endif // VK_GOOGLE_display_timing
    }

    VkResult GetRefreshCycle(VkDevice device, VkSwapchainKHR swapchain, uint64_t* pRefreshDuration) {

        if (!vkGetRefreshCycleDurationGOOGLE) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkRefreshCycleDurationGOOGLE  displayTimingProperties = VkRefreshCycleDurationGOOGLE();
        VkResult result = vkGetRefreshCycleDurationGOOGLE(device, swapchain, &displayTimingProperties);
        if (VK_SUCCESS == result) {
            *pRefreshDuration = displayTimingProperties.refreshDuration;
        }
        return result;
    }

    bool DisplayTimingIsEnabled() {
        return (vkGetRefreshCycleDurationGOOGLE && vkGetPastPresentationTimingGOOGLE);
    }

    operator bool() {
        return DisplayTimingIsEnabled();
    }

private:
    PFN_vkGetRefreshCycleDurationGOOGLE   vkGetRefreshCycleDurationGOOGLE;
    PFN_vkGetPastPresentationTimingGOOGLE vkGetPastPresentationTimingGOOGLE;

};

class VulkanSwapchainInfo {

public:
    VulkanSwapchainInfo()
      : mInstance(),
        m_vkDevCtx(),
        mSurface(),
        mSwapchain(),
        mSwapchainNumBufs(),
        mDisplaySize(),
        mDisplayFormat(),
        mDisplayImages(nullptr),
        mPresentCompleteSemaphoresMem(nullptr),
        mPresentCompleteSemaphoreInFly(nullptr),
        mPresentCompleteSemaphores(),
        mDisplayTiming(m_vkDevCtx)
    { }

    void CreateSwapChain(const VulkanDeviceContext* vkDevCtx, VkSwapchainKHR swapchain);

    ~VulkanSwapchainInfo()
    {
        delete[] mDisplayImages;
        mDisplayImages = nullptr;

        if (mSwapchain) {
            m_vkDevCtx->DestroySwapchainKHR(*m_vkDevCtx,
                              mSwapchain, nullptr);
        }

        if (mSurface) {
            m_vkDevCtx->DestroySurfaceKHR(mInstance, mSurface, nullptr);
            mSurface = VkSurfaceKHR();
        }

        if (mPresentCompleteSemaphoresMem) {
            mPresentCompleteSemaphores.clear();
            mPresentCompleteSemaphoreInFly = nullptr;

            for (uint32_t i = 0; i < mSwapchainNumBufs + 1; i++) {
                m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, mPresentCompleteSemaphoresMem[i], nullptr);
            }

            delete[] mPresentCompleteSemaphoresMem;
            mPresentCompleteSemaphoresMem = nullptr;
        }

        mInstance = VkInstance();
        m_vkDevCtx = nullptr;
        mSwapchain = VkSwapchainKHR();
        mSwapchainNumBufs = 0;
        mSurface = VkSurfaceKHR();
        mDisplaySize = VkExtent2D();
        mDisplayFormat = VkFormat();
    }

    VkImage GetImage(uint32_t fbImageIndex) const {
        if (fbImageIndex < mSwapchainNumBufs) {
            return mDisplayImages[fbImageIndex];
        };

        return VkImage(0);
    }

    VkFormat GetImageFormat() const {
        return mDisplayFormat;
    }

    const VkExtent2D GetExtent2D() const {
        return mDisplaySize;
    }

    VkSemaphore* GetPresentSemaphoreInFly() {
        assert(mPresentCompleteSemaphoreInFly);

        return mPresentCompleteSemaphoreInFly;
    }

    void SetPresentSemaphoreInFly(uint32_t scIndex, VkSemaphore* semaphore)
    {
        assert(mPresentCompleteSemaphoreInFly == semaphore);
        assert(scIndex < mSwapchainNumBufs);

        // Swap the semaphore on the fly with the one that is requested to be set.
        VkSemaphore* tempSem = mPresentCompleteSemaphores[scIndex];
        mPresentCompleteSemaphores[scIndex] = mPresentCompleteSemaphoreInFly;
        mPresentCompleteSemaphoreInFly = tempSem;
    }

    VkSemaphore* GetPresentSemaphore(uint32_t scIndex)
    {
        VkSemaphore* tempSem = mPresentCompleteSemaphores[scIndex];
        assert(tempSem);
        return tempSem;
    }

    VkResult GetDisplayRefreshCycle(uint64_t* pRefreshDuration) {
        return mDisplayTiming.GetRefreshCycle(*m_vkDevCtx, mSwapchain, pRefreshDuration);
    }

    VkInstance mInstance;
    const VulkanDeviceContext* m_vkDevCtx;
    VkSurfaceKHR mSurface;
    VkSwapchainKHR mSwapchain;
    uint32_t mSwapchainNumBufs;

    VkExtent2D mDisplaySize;
    VkFormat mDisplayFormat;

    // array of frame buffers and views
    VkImage *mDisplayImages;

    VkSemaphore* mPresentCompleteSemaphoresMem;
    VkSemaphore* mPresentCompleteSemaphoreInFly;
    std::vector <VkSemaphore*> mPresentCompleteSemaphores;

    VulkanDisplayTiming mDisplayTiming;
};

class ImageObject : public ImageResourceInfo {
public:
    ImageObject ()
    :   ImageResourceInfo(),
        m_vkDevCtx()
    { }

    VkResult CreateImage(const VulkanDeviceContext* vkDevCtx,
            const VkImageCreateInfo* pImageCreateInfo,
            VkMemoryPropertyFlags requiredMemProps = 0,
            int initWithPattern = -1) {

        DestroyImage();

        VkResult result = VkImageResource::Create(vkDevCtx,
                                                  pImageCreateInfo,
                                                  requiredMemProps,
                                                  m_imageResource);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        result = VkImageResourceView::Create(vkDevCtx, m_imageResource,
                                             subresourceRange,
                                             m_imageView);

        if (result != VK_SUCCESS) {
            return result;
        }

        m_vkDevCtx = vkDevCtx;

        image       = m_imageResource->GetImage();
        view        = m_imageView->GetImageView();
        imageFormat = pImageCreateInfo->format;
        imageWidth =  pImageCreateInfo->extent.width;
        imageHeight = pImageCreateInfo->extent.height;
        imageLayout = pImageCreateInfo->initialLayout;

        if (initWithPattern && (requiredMemProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            FillImageWithPattern(initWithPattern);
        }

        return result;
    }

    VkResult AllocMemoryAndBind(const VulkanDeviceContext* vkDevCtx, VkImage vkImage, VkDeviceMemory& imageDeviceMemory, VkMemoryPropertyFlags requiredMemProps,
            bool dedicated, VkExternalMemoryHandleTypeFlags exportMemHandleTypes, NativeHandle& importHandle = NativeHandle::InvalidNativeHandle);

    VkResult FillImageWithPattern(int pattern);
    VkResult CopyYuvToVkImage(uint32_t numPlanes, const uint8_t* yuvPlaneData[3], const VkSubresourceLayout yuvPlaneLayouts[3]);
    VkResult StageImage(const VulkanDeviceContext* vkDevCtx, VkImageUsageFlags usage, VkMemoryPropertyFlags requiredMemProps, bool needBlit);

    VkResult GetMemoryFd(int* pFd) const;
    int32_t GetImageSubresourceAndLayout(VkSubresourceLayout layouts[3]) const;

    ImageObject& operator= (const ImageObject&) = delete;
    ImageObject& operator= (ImageObject&&) = delete;

    uint8_t* MapHostPtr() {
        VkDeviceSize maxSize = VK_WHOLE_SIZE;
        return m_imageResource->GetMemory()->GetDataPtr(0, maxSize);
    }

    operator bool() {
        return (m_imageResource && (m_imageResource->GetImage() != VK_NULL_HANDLE));
    }

    ~ImageObject()
    {
        DestroyImage();
    }

    void DestroyImage()
    {
        m_imageView     = nullptr;
        m_imageResource = nullptr;
    }

    const VulkanDeviceContext* m_vkDevCtx;
    VkSharedBaseObj<VkImageResource>     m_imageResource;
    VkSharedBaseObj<VkImageResourceView> m_imageView;
};

class VulkanSamplerYcbcrConversion {

public:

    VulkanSamplerYcbcrConversion ()
        : m_vkDevCtx(),
          mSamplerInfo(),
          mSamplerYcbcrConversionCreateInfo(),
          mSamplerYcbcrConversion(),
          sampler()
    {

    }

    ~VulkanSamplerYcbcrConversion () {
        DestroyVulkanSampler();
    }

    void DestroyVulkanSampler() {
        if (sampler) {
            m_vkDevCtx->DestroySampler(*m_vkDevCtx, sampler, nullptr);
        }
        sampler = VkSampler();

        if(mSamplerYcbcrConversion) {
            m_vkDevCtx->DestroySamplerYcbcrConversion(*m_vkDevCtx, mSamplerYcbcrConversion, NULL);
        }

        mSamplerYcbcrConversion = VkSamplerYcbcrConversion(0);
    }

    VkResult CreateVulkanSampler(const VulkanDeviceContext* vkDevCtx,
            const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

    VkSampler GetSampler() const {
      return sampler;
    }

    const VkSamplerYcbcrConversionCreateInfo& GetSamplerYcbcrConversionCreateInfo() const
    {
        return mSamplerYcbcrConversionCreateInfo;
    }

    // sampler requires update if the function were to return true.
    bool SamplerRequiresUpdate(const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkSamplerCreateInfo mSamplerInfo;
    VkSamplerYcbcrConversionCreateInfo mSamplerYcbcrConversionCreateInfo;
    VkSamplerYcbcrConversion mSamplerYcbcrConversion;
    VkSampler sampler;
};

class VulkanRenderPass {

public:
    VulkanRenderPass()
        : m_vkDevCtx(),
          renderPass()
    {}

    VkResult CreateRenderPass(const VulkanDeviceContext* vkDevCtx, VkFormat displayImageFormat);

    void DestroyRenderPass() {
        if (renderPass) {
            m_vkDevCtx->DestroyRenderPass(*m_vkDevCtx,
                            renderPass, nullptr);

            renderPass = VkRenderPass(0);
        }
    }

    ~VulkanRenderPass() {
        DestroyRenderPass();
    }

    VkRenderPass getRenderPass() {
        return renderPass;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkRenderPass renderPass;
};

class VulkanVertexBuffer {

public:
    VulkanVertexBuffer()
        : m_vertexBuffer(nullptr) { }

    VkBuffer GetBuffer() const {
        return m_vertexBuffer->GetBuffer();
    }

    VkResult CreateVertexBuffer(const VulkanDeviceContext* vkDevCtx,  const float* pVertexData,
                                VkDeviceSize vertexDataSize, uint32_t numVertices)
    {
        DestroyVertexBuffer();

        uint32_t queueFamilyIndex = vkDevCtx->GetGfxQueueFamilyIdx();
        return VkBufferResource::Create(vkDevCtx,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                        vertexDataSize,
                                        m_vertexBuffer, 1, 1,
                                        vertexDataSize, pVertexData,
                                        1, &queueFamilyIndex);
    }


    void DestroyVertexBuffer()
    {
        m_vertexBuffer   = nullptr;
    }

    ~VulkanVertexBuffer()
    {
        DestroyVertexBuffer();
    }

    uint32_t GetNumVertices() const {
        return 4;
    }

private:
    VkSharedBaseObj<VkBufferResource>  m_vertexBuffer;
};

class VulkanFrameBuffer {

public:
    VulkanFrameBuffer()
       : m_vkDevCtx(),
         mFbImage(),
         mImageView(),
         mFramebuffer() {}

    ~VulkanFrameBuffer()
    {
        DestroyFrameBuffer();
    }

    void DestroyFrameBuffer()
    {
        if (mFramebuffer) {
            m_vkDevCtx->DestroyFramebuffer(*m_vkDevCtx, mFramebuffer, nullptr);
            mFramebuffer = VkFramebuffer(0);
        }

        if (mImageView) {
            m_vkDevCtx->DestroyImageView(*m_vkDevCtx, mImageView, nullptr);
            mImageView = VkImageView(0);
        }

        mFbImage = VkImage();
    }

    VkFramebuffer GetFrameBuffer()
    {
        return mFramebuffer;
    }

    VkImage GetFbImage()
    {
        return mFbImage;
    }

    VkResult CreateFrameBuffer(const VulkanDeviceContext* vkDevCtx, VkSwapchainKHR swapchain,
            const VkExtent2D* pExtent2D, const VkSurfaceFormatKHR* pSurfaceFormat, VkImage fbImage,
            VkRenderPass renderPass, VkImageView depthView = VK_NULL_HANDLE);

    const VulkanDeviceContext* m_vkDevCtx;
    VkImage       mFbImage;
    VkImageView   mImageView;
    VkFramebuffer mFramebuffer;
};


class VulkanSyncPrimitives {

public:
    VulkanSyncPrimitives()
       : m_vkDevCtx(),
         mRenderCompleteSemaphore(),
         mFence() {}

    ~VulkanSyncPrimitives()
    {
        DestroySyncPrimitives();
    }

    void DestroySyncPrimitives()
    {
        if (mFence) {
            m_vkDevCtx->DestroyFence(*m_vkDevCtx, mFence, nullptr);
            mFence = VkFence();
        }

        if (mRenderCompleteSemaphore) {
            m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, mRenderCompleteSemaphore, nullptr);
            mRenderCompleteSemaphore = VkSemaphore();
        }
    }

    VkResult CreateSyncPrimitives(const VulkanDeviceContext* vkDevCtx);

    const VulkanDeviceContext* m_vkDevCtx;
    VkSemaphore   mRenderCompleteSemaphore;
    VkFence       mFence;
};

class VulkanDescriptorSet {

public:

    VulkanDescriptorSet(const VulkanDeviceContext* vkDevCtx = nullptr)
        : m_vkDevCtx(vkDevCtx), m_descPool(), m_descSet() {}

    ~VulkanDescriptorSet()
    {
        DestroyDescriptorSets();
        DestroyDescriptorPool();
    }

    void DestroyDescriptorSets()
    {
        if (m_descSet) {
            m_vkDevCtx->FreeDescriptorSets(*m_vkDevCtx, m_descPool, 1, &m_descSet);
            m_descSet = VkDescriptorSet(0);
        }
    }

    void DestroyDescriptorPool()
    {
        if (m_descPool) {
            m_vkDevCtx->DestroyDescriptorPool(*m_vkDevCtx, m_descPool, nullptr);
            m_descPool = VkDescriptorPool(0);
        }
    }

    VkResult CreateDescriptorPool(const VulkanDeviceContext* vkDevCtx,
                                  uint32_t descriptorCount,
                                  VkDescriptorType descriptorType)
    {

        DestroyDescriptorPool();

        m_vkDevCtx = vkDevCtx;

        VkDescriptorPoolSize descriptorPoolSize = VkDescriptorPoolSize();
        descriptorPoolSize.type = descriptorType;
        descriptorPoolSize.descriptorCount = descriptorCount;

        VkDescriptorPoolCreateInfo descriptor_pool = VkDescriptorPoolCreateInfo();
        descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool.pNext = nullptr;
        descriptor_pool.maxSets = 1;
        descriptor_pool.poolSizeCount = 1;
        descriptor_pool.pPoolSizes = &descriptorPoolSize;
        return m_vkDevCtx->CreateDescriptorPool(*m_vkDevCtx, &descriptor_pool, nullptr, &m_descPool);
    }

    VkResult AllocateDescriptorSets(uint32_t descriptorCount,
                                    VkDescriptorSetLayout* dscLayout)
    {
        assert(m_vkDevCtx);
        DestroyDescriptorSets();

        VkDescriptorSetAllocateInfo alloc_info = VkDescriptorSetAllocateInfo();
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.pNext = nullptr;
        alloc_info.descriptorPool = m_descPool;
        alloc_info.descriptorSetCount = descriptorCount;
        alloc_info.pSetLayouts = dscLayout;
        VkResult result = m_vkDevCtx->AllocateDescriptorSets(*m_vkDevCtx, &alloc_info, &m_descSet);

        return result;
    }

    const VkDescriptorSet* GetDescriptorSet() const {
        return &m_descSet;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkDescriptorPool m_descPool;
    VkDescriptorSet  m_descSet;
};

class VulkanDescriptorSetLayoutBinding {

    enum { MAX_DESCRIPTOR_SET_POOLS = 4 };

public:
    VulkanDescriptorSetLayoutBinding()
     : m_vkDevCtx(),
       descriptorSetLayoutBinding(),
       descriptorSetLayoutCreateInfo(),
       dscLayout(),
       pipelineLayout(),
       currentDescriptorSetPools(-1),
       descSets{},
       m_descriptorOffset{},
       m_descriptorSize{}
    { }

    ~VulkanDescriptorSetLayoutBinding()
    {
        DestroyPipelineLayout();
        DestroyDescriptorSetLayout();
    }

    void DestroyPipelineLayout()
    {
        if (pipelineLayout) {
            m_vkDevCtx->DestroyPipelineLayout(*m_vkDevCtx, pipelineLayout, nullptr);
            pipelineLayout = VkPipelineLayout(0);
        }
    }

    void DestroyDescriptorSetLayout()
    {
        m_resourceDescriptorBuffer = nullptr;

        if (dscLayout) {
            m_vkDevCtx->DestroyDescriptorSetLayout(*m_vkDevCtx, dscLayout, nullptr);
            dscLayout = VkDescriptorSetLayout(0);
        }
    }

    // initialize descriptor set
    VkResult CreateDescriptorSet(const VulkanDeviceContext* vkDevCtx,
                                 uint32_t descriptorCount = 1,
                                 uint32_t maxCombinedImageSamplerDescriptorCount = 4,
                                 const VkSampler* pImmutableSamplers = nullptr);

    // initialize descriptor set
    VkResult CreateDescriptorSet(const VulkanDeviceContext* vkDevCtx, VkDescriptorPool allocPool, VkDescriptorSetLayout* dscLayouts, uint32_t descriptorSetCount = 1);

    VkResult WriteDescriptorSet(VkSampler sampler, VkImageView imageView, uint32_t dstArrayElement = 0,
                                VkImageLayout imageLayout = VK_IMAGE_LAYOUT_GENERAL);

    VkResult CreateFragmentShaderOutput(VkDescriptorType outMode, uint32_t outSet, uint32_t outBinding, uint32_t outArrayIndex, std::stringstream& imageFss);

    VkResult CreateFragmentShaderLayouts(const uint32_t* setIds, uint32_t numSets, std::stringstream& texFss);


    const VkDescriptorSet* GetDescriptorSet() const {
        if (currentDescriptorSetPools < 0) {
            return nullptr;
        }
        return descSets[currentDescriptorSetPools].GetDescriptorSet();
    }


    bool UsesDescriptorBuffer() const {
        return m_resourceDescriptorBuffer;
    }

    VkDescriptorSetLayoutCreateFlags GetDescriptorLayoutMode() const {
        // VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR or
        // VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
        return descriptorSetLayoutCreateInfo.flags;
    }
    VkDeviceOrHostAddressConstKHR UpdateDescriptorBuffer(uint32_t descriptorId,
                                                         const VkDescriptorImageInfo* pCombinedImageSampler) const {

        assert(descriptorId == 0); // Only one resource is currently supported

        // Set descriptors for image
        VkDeviceSize dstBufferOffset = 0;
        VkDeviceSize maxSize = 0;
        uint8_t* imageDescriptorBufPtr = const_cast<uint8_t*>((const uint8_t*)m_resourceDescriptorBuffer->GetDataPtr(dstBufferOffset, maxSize));
        assert(imageDescriptorBufPtr);

        VkDescriptorGetInfoEXT descriptorInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
        descriptorInfo.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorInfo.data.pCombinedImageSampler = pCombinedImageSampler;
        m_vkDevCtx->GetDescriptorEXT(*m_vkDevCtx, &descriptorInfo, m_descriptorSize[descriptorId],
                                     imageDescriptorBufPtr + 0 /* index */ * m_descriptorOffset[descriptorId]);

        VkBuffer imageDescriptorBuffer = m_resourceDescriptorBuffer->GetBuffer();
        assert(imageDescriptorBuffer);
        VkDeviceOrHostAddressConstKHR imageDescriptorBufferDeviceAddress;
        VkBufferDeviceAddressInfoKHR bufferDeviceAddressInfo{};
        bufferDeviceAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bufferDeviceAddressInfo.buffer = imageDescriptorBuffer;
        imageDescriptorBufferDeviceAddress.deviceAddress = m_vkDevCtx->GetBufferDeviceAddressKHR(*m_vkDevCtx, &bufferDeviceAddressInfo);

        return imageDescriptorBufferDeviceAddress;
    }


    VulkanDescriptorSet* GetNextDescriptorSet() {
        currentDescriptorSetPools++;
        currentDescriptorSetPools = currentDescriptorSetPools % MAX_DESCRIPTOR_SET_POOLS;
        assert((currentDescriptorSetPools >= 0) && (currentDescriptorSetPools < MAX_DESCRIPTOR_SET_POOLS));
        return &descSets[currentDescriptorSetPools];
    }

    const VkDescriptorSetLayout* GetDescriptorSetLayout() const {
        return &dscLayout;
    }

    VkPipelineLayout GetPipelineLayout() const {
        return pipelineLayout;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkDescriptorSetLayoutBinding descriptorSetLayoutBinding;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    VkDescriptorSetLayout dscLayout;
    VkPipelineLayout pipelineLayout;
    int32_t currentDescriptorSetPools;
    VulkanDescriptorSet descSets[MAX_DESCRIPTOR_SET_POOLS];
    VkSharedBaseObj<VkBufferResource>  m_resourceDescriptorBuffer; // If VK_EXT_descriptor_buffer is supported
    VkDeviceSize  m_descriptorOffset[1]; // Only one for now, since there is only one image descriptor
    size_t        m_descriptorSize[1];   // Only one for now, since there is only one image descriptor
};

class VulkanGraphicsPipeline {

public:
    VulkanGraphicsPipeline()
     : m_vkDevCtx(),
       cache(),
       pipeline(),
       mVulkanShaderCompiler(),
       mFssCache(),
       mVertexShaderCache(0),
       mFragmentShaderCache(0)
    { }

    ~VulkanGraphicsPipeline()
    {
        DestroyPipeline();
        DestroyVertexShaderModule();
        DestroyFragmentShaderModule();
        DestroyPipelineCache();
    }

    // Destroy Graphics Pipeline
    void DestroyPipeline()
    {
        if (pipeline) {
            m_vkDevCtx->DestroyPipeline(*m_vkDevCtx, pipeline, nullptr);
            pipeline = VkPipeline(0);
        }
    }

    void DestroyPipelineCache()
    {
        if (cache) {
            m_vkDevCtx->DestroyPipelineCache(*m_vkDevCtx, cache, nullptr);
            cache = VkPipelineCache(0);
        }
    }

    void DestroyVertexShaderModule()
    {
        if (mVertexShaderCache) {
            m_vkDevCtx->DestroyShaderModule(*m_vkDevCtx, mVertexShaderCache, nullptr);
            mVertexShaderCache = VkShaderModule();
        }
    }

    void DestroyFragmentShaderModule()
    {
        if (mFragmentShaderCache) {
            m_vkDevCtx->DestroyShaderModule(*m_vkDevCtx, mFragmentShaderCache, nullptr);
            mFragmentShaderCache = VkShaderModule();
        }
    }

    // Create Graphics Pipeline
    VkResult CreateGraphicsPipeline(const VulkanDeviceContext* vkDevCtx, VkViewport* pViewport, VkRect2D* pScissor,
            VkRenderPass renderPass, VulkanDescriptorSetLayoutBinding* pBufferDescriptorSets);

    VkPipeline getPipeline() {
        return pipeline;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkPipelineCache cache;
    VkPipeline pipeline;
    VulkanShaderCompiler mVulkanShaderCompiler;
    std::stringstream mFssCache;
    VkShaderModule mVertexShaderCache;
    VkShaderModule mFragmentShaderCache;
};

class VulkanCommandBuffer {

public:

    VulkanCommandBuffer()
        :m_vkDevCtx(),
        cmdPool(),
        cmdBuffer()
        {}

    VkResult CreateCommandBufferPool(const VulkanDeviceContext* vkDevCtx);

    VkResult CreateCommandBuffer(VkRenderPass renderPass,
            const ImageResourceInfo* inputImageToDrawFrom,
            int32_t displayWidth, int32_t displayHeight,
            VkImage displayImage, VkFramebuffer framebuffer, VkRect2D* pRenderArea,
            VkPipeline pipeline,
            const VulkanDescriptorSetLayoutBinding& descriptorSetLayoutBinding,
            const VulkanSamplerYcbcrConversion& samplerYcbcrConversion,
            const VulkanVertexBuffer& vertexBuffer);

    ~VulkanCommandBuffer() {
        DestroyCommandBuffer();
        DestroyCommandBufferPool();
    }

    void DestroyCommandBuffer() {
        if (cmdBuffer) {
            m_vkDevCtx->FreeCommandBuffers(*m_vkDevCtx, cmdPool, 1, &cmdBuffer);
            cmdBuffer = VkCommandBuffer(0);
        }
    }

    void DestroyCommandBufferPool() {
        if (cmdPool) {
           m_vkDevCtx->DestroyCommandPool(*m_vkDevCtx, cmdPool, nullptr);
           cmdPool = VkCommandPool(0);
        }
    }

    VkCommandPool getCommandPool() {
        return cmdPool;
    }

    const VkCommandBuffer* getCommandBuffer() {
        return &cmdBuffer;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkCommandPool cmdPool;
    VkCommandBuffer cmdBuffer;
};

class VulkanPerDrawContext {

public:
    VulkanPerDrawContext()
    : contextIndex(-1),
      frameBuffer(),
      syncPrimitives(),
      samplerYcbcrConversion(),
      descriptorSetLayoutBinding(),
      commandBuffer(),
      gfxPipeline(),
      lastVideoFormatUpdate((uint32_t)-1)
    {
    }

    ~VulkanPerDrawContext() {
    }

    bool IsFormatOutOfDate(uint32_t formatUpdateCounter) {
        if (formatUpdateCounter != lastVideoFormatUpdate) {
            lastVideoFormatUpdate = formatUpdateCounter;
            return true;
        }
        return false;
    }

    int32_t contextIndex;
    VulkanFrameBuffer frameBuffer;
    VulkanSyncPrimitives syncPrimitives;
    VulkanSamplerYcbcrConversion samplerYcbcrConversion;
    VulkanDescriptorSetLayoutBinding descriptorSetLayoutBinding;
    VulkanCommandBuffer commandBuffer;
    VulkanGraphicsPipeline gfxPipeline;
    uint32_t lastVideoFormatUpdate;
};

class VulkanRenderInfo {

public:

    VulkanRenderInfo()
      : currentBuffer(0),
        lastBuffer(0xFFFFFFFF),
        lastRealTimeNsecs(0),
        frameTimeNsecs(0),
        totalFrames(0),
        skippedFrames(0),
        frameId(0),
        m_vkDevCtx(),
        mNumCtxs(0),
        perDrawCtx(nullptr)
        {}


    // Create per draw contexts.
    VkResult CreatePerDrawContexts(const VulkanDeviceContext* vkDevCtx,
            VkSwapchainKHR swapchain, const VkExtent2D* pFbExtent2D,
            VkViewport* pViewport, VkRect2D* pScissor, const VkSurfaceFormatKHR* pSurfaceFormat,
            VkRenderPass renderPass, const VkSamplerCreateInfo* pSamplerCreateInfo = nullptr,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo = nullptr);

    VkResult UpdatePerDrawContexts(VulkanPerDrawContext* pPerDrawContext,
            VkViewport* pViewport, VkRect2D* pScissor, VkRenderPass renderPass,
            const VkSamplerCreateInfo* pSamplerCreateInfo = nullptr,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo = nullptr);

    uint32_t GetNumDrawContexts() const {
        return mNumCtxs;
    }

    VulkanPerDrawContext* GetDrawContext(int32_t scIndx) {
        return (scIndx < mNumCtxs) ? &perDrawCtx[scIndx] : nullptr;
    }

    VkResult WaitCurrentSwapcahinDraw(VulkanSwapchainInfo* pSwapchainInfo, VulkanPerDrawContext* pPerDrawContext,
            uint64_t timeoutNsec = 100000000);

    int32_t GetNextSwapchainBuffer(
            VulkanSwapchainInfo* pSwapchainInfo, VulkanPerDrawContext* pPerDrawContext, uint64_t timeoutNsec = 100000000);

    // Draw one frame
    VkResult DrawFrame(const VulkanDeviceContext* vkDevCtx,
            VulkanSwapchainInfo* pSwapchainInfo, int64_t presentTimestamp,
            VulkanPerDrawContext* pPerDrawContext,
            uint32_t commandBufferCount = 1);

    ~VulkanRenderInfo() {

        if (perDrawCtx) {
            delete [] perDrawCtx;
            perDrawCtx = nullptr;
        }
    }

    uint64_t GotFrame() {
        return ++totalFrames;
    }

    uint64_t GetTotalFrames() {
        return totalFrames;
    }

    uint32_t SkippedFrame() {
        return ++skippedFrames;
    }

    uint32_t getframeId() {
        return frameId;
    }

private:
    uint32_t currentBuffer;
    uint32_t lastBuffer;
    uint64_t lastRealTimeNsecs;
    uint64_t frameTimeNsecs;
    uint64_t totalFrames;
    uint32_t skippedFrames;
    uint32_t frameId;
    const VulkanDeviceContext* m_vkDevCtx;
    int32_t mNumCtxs;
    VulkanPerDrawContext* perDrawCtx;

};

class VkVideoAppCtx {

public:
    bool                       m_initialized;
    // WindowInfo window;
    const VulkanDeviceContext* m_vkDevCtx;
    bool                       m_useTestImage;
    ImageObject                m_testFrameImage;
    VulkanRenderPass           m_renderPass;
    VulkanVertexBuffer         m_vertexBuffer;
    VulkanRenderInfo           m_renderInfo;

    ~VkVideoAppCtx()
    {
        if (!m_initialized) {
            return;
        }

        m_initialized = false;
    }

    VkVideoAppCtx(bool testVk)
        : m_initialized(false),
          // window(),
          m_vkDevCtx(),
          m_useTestImage(testVk),
          // swapchain_(),
          m_renderPass(),
          m_vertexBuffer(),
          m_renderInfo()
    {
        CreateSamplerYcbcrConversions();
    }

    VkResult CreateSamplerYcbcrConversions();

    void ContextIsReady() {
        m_initialized = true;
    }

    bool IsContextReady() const {
        return m_initialized;
    }

};

// A helper functions
// A helper function to map required memory property into a VK memory type
// memory type is an index into the array of 32 entries; or the bit index
// for the memory type ( each BIT of an 32 bit integer is a type ).
VkResult AllocateMemoryTypeFromProperties(const VulkanDeviceContext* vkDevCtx,
        uint32_t typeBits,
        VkFlags requirements_mask,
        uint32_t *typeIndex);

void setImageLayout(const VulkanDeviceContext* vkDevCtx,
                    VkCommandBuffer cmdBuffer, VkImage image,
                    VkImageLayout oldImageLayout, VkImageLayout newImageLayout,
                    VkPipelineStageFlags srcStages,
                    VkPipelineStageFlags destStages, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

} // End of namespace vulkanVideoUtils

#endif // __VULKANVIDEOUTILS__
