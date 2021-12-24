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

#if defined(__unix__)
#include <unistd.h>
#endif

#include <vector>
#include <iostream>     // std::cout
#include <sstream>      // std::stringstream
#include <algorithm>    // std::find_if

#include <glm/glm.hpp>

#ifndef __VULKANVIDEOUTILS__
#define __VULKANVIDEOUTILS__

#include <vulkan_interfaces.h>

namespace vulkanVideoUtils {

struct Vertex {
    float position[2];
    float texCoord[2];
};

struct TransformPushConstants {
    glm::mat4 posMatrix;
    glm::mat2 texMatrix;
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
    // NativeHandle& operator= (const NativeHandle&);
    NativeHandle& operator= (const NativeHandle&) = delete;
};

// Global Variables ...
class VulkanDeviceInfo {

public:

    VulkanDeviceInfo(VkInstance instance = VkInstance(),
            VkPhysicalDevice physDevice = VkPhysicalDevice(),
            VkDevice device = VkDevice(),
            uint32_t queueFamilyIndex = -1,
            VkQueue queue = VkQueue())
    :   instance_(instance),
        physDevice_(physDevice),
        device_(device),
        queueFamilyIndex_(queueFamilyIndex),
        queue_(queue),
        attached_(device != VkDevice()),
        gpuMemoryProperties_()
    {

    }

    // Create vulkan device
    void CreateVulkanDevice(VkApplicationInfo *appInfo);
    void AttachVulkanDevice(VkInstance instance,
                            VkPhysicalDevice physDevice,
                            VkDevice device,
                            uint32_t queueFamilyIndex = -1,
                            VkQueue queue = VkQueue(),
                            VkPhysicalDeviceMemoryProperties* pMemoryProperties = NULL);

    VkInstance getInstance() {
        return instance_;
    }

    VkDevice getDevice() {
        return device_;
    }

    operator VkDevice() const { return device_; }

    void DeviceWaitIdle()
    {
        vk::DeviceWaitIdle(device_);
    }

    ~VulkanDeviceInfo() {

        if (device_) {
            if (!attached_) {
                vk::DestroyDevice(device_, nullptr);
            }
            device_ = VkDevice();
        }

        if (instance_) {
            if (!attached_) {
                vk::DestroyInstance(instance_, nullptr);
            }
            instance_ = VkInstance();
        }

        queue_ = VkQueue();
        attached_ = false;
    }

    const VkExtensionProperties* FindExtension(
        const std::vector<VkExtensionProperties>& extensions,
        const char* name) {
        auto it = std::find_if(extensions.cbegin(), extensions.cend(),
                               [=](const VkExtensionProperties& ext) {
                                   return (strcmp(ext.extensionName, name) == 0);
                               });
        return (it != extensions.cend()) ? &*it : nullptr;
    }

    const VkExtensionProperties* FindInstanceExtension(const char* name) {
        return FindExtension(instanceExtensions_, name);
    }

    const VkExtensionProperties* FindDeviceExtension(const char* name) {
        return FindExtension(deviceExtensions_, name);
    }

    void PrintExtensions(bool deviceExt = false) {
        const std::vector<VkExtensionProperties>& extensions = deviceExt ? deviceExtensions_ : instanceExtensions_;
        for (const auto& e : extensions)
            printf("\t%s (v%u)\n", e.extensionName, e.specVersion);
    }

private:

    bool PopulateInstanceExtensions() {
      uint32_t extensions_count = 0;
      VkResult result = VK_SUCCESS;

      result = vk::EnumerateInstanceExtensionProperties( nullptr, &extensions_count, nullptr );
      if( (result != VK_SUCCESS) ||
          (extensions_count == 0) ) {
        std::cout << "Could not get the number of instance extensions." << std::endl;
        return false;
      }

      instanceExtensions_.resize( extensions_count );
      result = vk::EnumerateInstanceExtensionProperties( nullptr, &extensions_count, instanceExtensions_.data() );
      if( (result != VK_SUCCESS) ||
          (extensions_count == 0) ) {
        std::cout << "Could not enumerate instance extensions." << std::endl;
        return false;
      }

      return true;
    }

    bool PopulateDeviceExtensions() {
      uint32_t extensions_count = 0;
      VkResult result = VK_SUCCESS;

      result = vk::EnumerateDeviceExtensionProperties( physDevice_, nullptr, &extensions_count, nullptr );
      if( (result != VK_SUCCESS) ||
          (extensions_count == 0) ) {
        std::cout << "Could not get the number of device extensions." << std::endl;
        return false;
      }

      deviceExtensions_.resize( extensions_count );
      result = vk::EnumerateDeviceExtensionProperties( physDevice_, nullptr, &extensions_count, deviceExtensions_.data() );
      if( (result != VK_SUCCESS) ||
          (extensions_count == 0) ) {
        std::cout << "Could not enumerate device extensions." << std::endl;
        return false;
      }

      return true;
    }

public:
    VkInstance instance_;
    VkPhysicalDevice physDevice_;
    VkDevice device_;
    uint32_t queueFamilyIndex_;
    VkQueue queue_;
    bool attached_;
    std::vector<VkExtensionProperties> instanceExtensions_;
    std::vector<VkExtensionProperties> deviceExtensions_;
    VkPhysicalDeviceMemoryProperties gpuMemoryProperties_;
};

class VulkanDisplayTiming  {
public:
    VulkanDisplayTiming(VkDevice device)
    : vkGetRefreshCycleDurationGOOGLE(nullptr),
      vkGetPastPresentationTimingGOOGLE(nullptr)
    {
#ifdef VK_GOOGLE_display_timing
    vkGetRefreshCycleDurationGOOGLE =
            reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
                    vk::GetDeviceProcAddr(device, "vkGetRefreshCycleDurationGOOGLE"));
    vkGetPastPresentationTimingGOOGLE =
            reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                    vk::GetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));

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
        m_device(),
        mSurface(),
        mSwapchain(),
        mSwapchainNumBufs(),
        mDisplaySize(),
        mDisplayFormat(),
        mDisplayImages(nullptr),
        mPresentCompleteSemaphoresMem(nullptr),
        mPresentCompleteSemaphoreInFly(nullptr),
        mPresentCompleteSemaphores(),
        mDisplayTiming(m_device)
    { }

    void CreateSwapChain(VulkanDeviceInfo* deviceInfo, VkSwapchainKHR swapchain);

    ~VulkanSwapchainInfo()
    {
        delete[] mDisplayImages;
        mDisplayImages = nullptr;

        if (mSwapchain) {
            vk::DestroySwapchainKHR(m_device,
                              mSwapchain, nullptr);
        }

        if (mSurface) {
            vk::DestroySurfaceKHR(mInstance, mSurface, nullptr);
            mSurface = VkSurfaceKHR();
        }

        if (mPresentCompleteSemaphoresMem) {
            mPresentCompleteSemaphores.clear();
            mPresentCompleteSemaphoreInFly = nullptr;

            for (uint32_t i = 0; i < mSwapchainNumBufs + 1; i++) {
                vk::DestroySemaphore(m_device, mPresentCompleteSemaphoresMem[i], nullptr);
            }

            delete[] mPresentCompleteSemaphoresMem;
            mPresentCompleteSemaphoresMem = nullptr;
        }

        mInstance = VkInstance();
        m_device = VkDevice();
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
        return mDisplayTiming.GetRefreshCycle(m_device, mSwapchain, pRefreshDuration);
    }

    VkInstance mInstance;
    VkDevice m_device;
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

class VulkanVideoBitstreamBuffer {

public:
    VulkanVideoBitstreamBuffer()
        : m_device(0), m_buffer(0), m_deviceMemory(0), m_bufferSize(0),
          m_bufferOffsetAlignment(0),
          m_bufferSizeAlignment(0) { }

    const VkBuffer& get() const {
        return m_buffer;
    }

    VkResult CreateVideoBitstreamBuffer(VkPhysicalDevice gpuDevice, VkDevice device, uint32_t queueFamilyIndex,
             VkDeviceSize bufferSize, VkDeviceSize bufferOffsetAlignment,  VkDeviceSize bufferSizeAlignment,
             const unsigned char* pBitstreamData = NULL, VkDeviceSize bitstreamDataSize = 0, VkDeviceSize dstBufferOffset = 0);

    VkResult CopyVideoBitstreamToBuffer(const unsigned char* pBitstreamData,
            VkDeviceSize bitstreamDataSize, VkDeviceSize &dstBufferOffset) const;

    void DestroyVideoBitstreamBuffer()
    {
        if (m_deviceMemory) {
            vk::FreeMemory(m_device, m_deviceMemory, nullptr);
            m_deviceMemory = VkDeviceMemory(0);
        }

        if (m_buffer) {
            vk::DestroyBuffer(m_device, m_buffer, nullptr);
            m_buffer = VkBuffer(0);
        }

        m_device = VkDevice(0);

        m_bufferSize = 0;
        m_bufferOffsetAlignment = 0;
        m_bufferSizeAlignment = 0;
    }

    ~VulkanVideoBitstreamBuffer()
    {
        DestroyVideoBitstreamBuffer();
    }

    VkDeviceSize GetBufferSize() const {
        return m_bufferSize;
    }

    VkDeviceSize GetBufferOffsetAlignment() const {
        return m_bufferOffsetAlignment;
    }

private:
    VkDevice        m_device;
    VkBuffer        m_buffer;
    VkDeviceMemory  m_deviceMemory;
    VkDeviceSize    m_bufferSize;
    VkDeviceSize    m_bufferOffsetAlignment;
    VkDeviceSize    m_bufferSizeAlignment;
};

class DeviceMemoryObject {
public:
    DeviceMemoryObject ()
    :   m_device(),
        memory(),
        nativeHandle(),
        canBeExported(false)
    { }

    VkResult AllocMemory(VkDevice device, VkMemoryRequirements* pMemoryRequirements, VkMemoryPropertyFlags requiredMemProps);

    ~DeviceMemoryObject()
    {
        DestroyDeviceMemory();
    }

    void DestroyDeviceMemory()
    {
        canBeExported = false;

        if (memory) {
            vk::FreeMemory(m_device,
                    memory, 0);
        }

        memory = VkDeviceMemory();
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    AHardwareBufferHandle ExportHandle();
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR)

    VkDevice m_device;
    VkDeviceMemory memory;
    NativeHandle nativeHandle; // as a reference to know if this is the same imported buffer.
    bool canBeExported;
};

class ImageObject {
public:
    ImageObject ()
    :   m_device(),
        imageFormat(VK_FORMAT_UNDEFINED),
        imageWidth(0),
        imageHeight(0),
        image(),
        mem(),
        view(),
        m_exportMemHandleTypes(VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM),
        nativeHandle(),
        canBeExported(false),
        inUseBySwapchain(false),
        imageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    { }

    VkResult CreateImage(VulkanDeviceInfo* deviceInfo,
            const VkImageCreateInfo* pImageCreateInfo,
            VkMemoryPropertyFlags requiredMemProps = 0,
            int initWithPattern = -1,
            VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes = VkExternalMemoryHandleTypeFlagBitsKHR(),
            NativeHandle& importHandle = NativeHandle::InvalidNativeHandle);

    VkResult AllocMemoryAndBind(VulkanDeviceInfo* deviceInfo, VkImage vkImage, VkDeviceMemory& imageDeviceMemory, VkMemoryPropertyFlags requiredMemProps,
            bool dedicated, VkExternalMemoryHandleTypeFlags exportMemHandleTypes, NativeHandle& importHandle = NativeHandle::InvalidNativeHandle);

    VkResult FillImageWithPattern(int pattern);
    VkResult CopyYuvToVkImage(uint32_t numPlanes, const uint8_t* yuvPlaneData[3], const VkSubresourceLayout yuvPlaneLayouts[3]);
    VkResult StageImage(VulkanDeviceInfo* deviceInfo, VkImageUsageFlags usage, VkMemoryPropertyFlags requiredMemProps, bool needBlit);

    VkResult GetMemoryFd(int* pFd) const;
    int32_t GetImageSubresourceAndLayout(VkSubresourceLayout layouts[3]) const;

    ImageObject& operator= (const ImageObject&) = delete;
    ImageObject& operator= (ImageObject&&) = delete;

    operator bool() {
        return (image != VkImage());
    }

    ~ImageObject()
    {
        DestroyImage();
    }

    void DestroyImage()
    {
        canBeExported = false;

        if (view) {
            vk::DestroyImageView(m_device,
                               view, nullptr);
        }

        if (mem) {
            vk::FreeMemory(m_device,
                         mem, 0);
        }

        if (image) {
            vk::DestroyImage(m_device,
                           image, nullptr);
        }

        image = VkImage ();
        mem = VkDeviceMemory();
        view = VkImageView();
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    AHardwareBufferHandle ExportHandle();
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR)

    VkDevice m_device;
    VkFormat imageFormat;
    int32_t imageWidth;
    int32_t imageHeight;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkExternalMemoryHandleTypeFlagBitsKHR m_exportMemHandleTypes;
    NativeHandle nativeHandle; // as a reference to know if this is the same imported buffer.
    bool canBeExported;
    bool inUseBySwapchain;
    VkImageLayout imageLayout;
};

class VulkanSamplerYcbcrConversion {

public:

    VulkanSamplerYcbcrConversion ()
        : m_device(),
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
            vk::DestroySampler(m_device, sampler, nullptr);
        }
        sampler = VkSampler();

        if(mSamplerYcbcrConversion) {
            vk::DestroySamplerYcbcrConversion(m_device, mSamplerYcbcrConversion, NULL);
        }

        mSamplerYcbcrConversion = VkSamplerYcbcrConversion(0);
    }

    VkResult CreateVulkanSampler(VkDevice device,
            const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

    VkSampler GetSampler() {
      return sampler;
    }

    // sampler requires update if the function were to return true.
    bool SamplerRequiresUpdate(const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo);

private:
    VkDevice m_device;
    VkSamplerCreateInfo mSamplerInfo;
    VkSamplerYcbcrConversionCreateInfo mSamplerYcbcrConversionCreateInfo;
    VkSamplerYcbcrConversion mSamplerYcbcrConversion;
    VkSampler sampler;
};

class VulkanShaderCompiler {

public:

    VulkanShaderCompiler();
    ~VulkanShaderCompiler();

    VkResult BuildGlslShader(const char *shaderCode, size_t shaderSize, VkShaderStageFlagBits type,
                                 VkDevice vkDevice, VkShaderModule *shaderOut);

    // Create VK shader module from given glsl shader file
    VkResult BuildShaderFromFile(const char *filePath, VkShaderStageFlagBits type,
                                 VkDevice vkDevice, VkShaderModule *shaderOut);

private:
    void* compilerHandle;
};

class VulkanRenderPass {

public:
    VulkanRenderPass()
        : m_device(),
          renderPass()
    {}

    VkResult CreateRenderPass(VulkanDeviceInfo* deviceInfo, VkFormat displayImageFormat);

    void DestroyRenderPass() {
        if (renderPass) {
            vk::DestroyRenderPass(m_device,
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
    VkDevice m_device;
    VkRenderPass renderPass;
};

class VulkanVertexBuffer {

public:
    VulkanVertexBuffer()
        : vertexBuffer(0), m_device(0), deviceMemory(0), numVertices(0) { }

    const VkBuffer& get() {
        return vertexBuffer;
    }

    VkResult CreateVertexBuffer(VulkanDeviceInfo* deviceInfo,  const float* pVertexData,
            VkDeviceSize vertexDataSize, uint32_t numVertices);


    void DestroyVertexBuffer()
    {
        if (deviceMemory) {
            vk::FreeMemory(m_device, deviceMemory, nullptr);
            deviceMemory = VkDeviceMemory(0);
        }

        if (vertexBuffer) {
            vk::DestroyBuffer(m_device, vertexBuffer, nullptr);
            vertexBuffer = VkBuffer(0);
        }

        m_device     = VkDevice(0);
        numVertices  = 0;
    }

    ~VulkanVertexBuffer()
    {
        DestroyVertexBuffer();
    }

    uint32_t GetNumVertices() {
        return 4;
    }

private:
    VkBuffer vertexBuffer;
    VkDevice m_device;
    VkDeviceMemory deviceMemory;
    uint32_t numVertices;
};

class VulkanFrameBuffer {

public:
    VulkanFrameBuffer()
       : m_device(),
         mImageView(),
         mFramebuffer() {}

    ~VulkanFrameBuffer()
    {
        DestroyFrameBuffer();
    }

    void DestroyFrameBuffer()
    {
        if (mFramebuffer) {
            vk::DestroyFramebuffer(m_device, mFramebuffer, nullptr);
            mFramebuffer = VkFramebuffer(0);
        }

        if (mImageView) {
            vk::DestroyImageView(m_device, mImageView, nullptr);
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

    VkResult CreateFrameBuffer(VkDevice device, VkSwapchainKHR swapchain,
            const VkExtent2D* pExtent2D, const VkSurfaceFormatKHR* pSurfaceFormat, VkImage fbImage,
            VkRenderPass renderPass, VkImageView depthView = VK_NULL_HANDLE);

    VkDevice      m_device;
    VkImage       mFbImage;
    VkImageView   mImageView;
    VkFramebuffer mFramebuffer;
};


class VulkanSyncPrimitives {

public:
    VulkanSyncPrimitives()
       : m_device(),
         mRenderCompleteSemaphore(),
         mFence() {}

    ~VulkanSyncPrimitives()
    {
        DestroySyncPrimitives();
    }

    void DestroySyncPrimitives()
    {
        if (mFence) {
            vk::DestroyFence(m_device, mFence, nullptr);
            mFence = VkFence();
        }

        if (mRenderCompleteSemaphore) {
            vk::DestroySemaphore(m_device, mRenderCompleteSemaphore, nullptr);
            mRenderCompleteSemaphore = VkSemaphore();
        }
    }

    VkResult CreateSyncPrimitives(VkDevice device);

    VkDevice      m_device;
    VkSemaphore   mRenderCompleteSemaphore;
    VkFence       mFence;
};

class VulkanDescriptorSet {

public:
    VulkanDescriptorSet()
     : m_device(),
       descriptorSetLayoutBinding(),
       descriptorSetLayoutCreateInfo(),
       dscLayout(),
       pipelineLayout(),
       descPool(),
       descSet(NULL)
    { }

    ~VulkanDescriptorSet()
    {
        DestroyDescriptorSets();
        DestroyDescriptorPool();
        DestroyPipelineLayout();
        DestroyDescriptorSetLayout();
    }

    void DestroyDescriptorSets()
    {
        if (descSet) {
            vk::FreeDescriptorSets(m_device, descPool, 1, &descSet);
            descSet = VkDescriptorSet(0);
        }
    }

    void DestroyDescriptorPool()
    {
        if (descPool) {
            vk::DestroyDescriptorPool(m_device, descPool, nullptr);
            descPool = VkDescriptorPool(0);
        }
    }

    void DestroyPipelineLayout()
    {
        if (pipelineLayout) {
            vk::DestroyPipelineLayout(m_device, pipelineLayout, nullptr);
            pipelineLayout = VkPipelineLayout(0);
        }
    }

    void DestroyDescriptorSetLayout()
    {
        if (dscLayout) {
            vk::DestroyDescriptorSetLayout(m_device, dscLayout, nullptr);
            dscLayout = VkDescriptorSetLayout(0);
        }
    }

    // initialize descriptor set
    VkResult CreateDescriptorSet(VkDevice device,
            uint32_t descriptorCount = 1, const VkSampler* pImmutableSamplers = nullptr);

    // initialize descriptor set
    VkResult CreateDescriptorSet(VulkanDeviceInfo* deviceInfo, VkDescriptorPool allocPool, VkDescriptorSetLayout* dscLayouts, uint32_t descriptorSetCount = 1);

    VkResult WriteDescriptorSet(VkSampler sampler, VkImageView imageView, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_GENERAL);

    VkResult CreateFragmentShaderOutput(VkDescriptorType outMode, uint32_t outSet, uint32_t outBinding, uint32_t outArrayIndex, std::stringstream& imageFss);

    VkResult CreateFragmentShaderLayouts(const uint32_t* setIds, uint32_t numSets, std::stringstream& texFss);


    const VkDescriptorSet* getDescriptorSet() {
        return &descSet;
    }

    const VkDescriptorSetLayout* getDescriptorSetLayout() {
        return &dscLayout;
    }

    VkPipelineLayout getPipelineLayout() {
        return pipelineLayout;
    }

private:
    VkDevice m_device;
    VkDescriptorSetLayoutBinding descriptorSetLayoutBinding;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    VkDescriptorSetLayout dscLayout;
    VkPipelineLayout pipelineLayout;
    VkDescriptorPool descPool;
    VkDescriptorSet  descSet;
};

class VulkanGraphicsPipeline {

public:
    VulkanGraphicsPipeline()
     : m_device(),
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
            vk::DestroyPipeline(m_device, pipeline, nullptr);
            pipeline = VkPipeline(0);
        }
    }

    void DestroyPipelineCache()
    {
        if (cache) {
            vk::DestroyPipelineCache(m_device, cache, nullptr);
            cache = VkPipelineCache(0);
        }
    }

    void DestroyVertexShaderModule()
    {
        if (mVertexShaderCache) {
            vk::DestroyShaderModule(m_device, mVertexShaderCache, nullptr);
            mVertexShaderCache = VkShaderModule();
        }
    }

    void DestroyFragmentShaderModule()
    {
        if (mFragmentShaderCache) {
            vk::DestroyShaderModule(m_device, mFragmentShaderCache, nullptr);
            mFragmentShaderCache = VkShaderModule();
        }
    }

    // Create Graphics Pipeline
    VkResult CreateGraphicsPipeline(VkDevice device, VkViewport* pViewport, VkRect2D* pScissor,
            VkRenderPass renderPass, VulkanDescriptorSet* pBufferDescriptorSets);

    VkPipeline getPipeline() {
        return pipeline;
    }

private:
    VkDevice m_device;
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
        :m_device(),
        cmdPool(),
        cmdBuffer()
        {}

    VkResult CreateCommandBufferPool(VulkanDeviceInfo* deviceInfo);

    VkResult CreateCommandBuffer(VkRenderPass renderPass, const ImageObject* inputImageToDrawFrom,
            int32_t displayWidth, int32_t displayHeight,
            VkImage displayImage, VkFramebuffer framebuffer, VkRect2D* pRenderArea,
            VkPipeline pipeline, VkPipelineLayout pipelineLayout, const VkDescriptorSet* pDescriptorSet,
            VulkanVertexBuffer* pVertexBuffer);

    ~VulkanCommandBuffer() {
        DestroyCommandBuffer();
        DestroyCommandBufferPool();
    }

    void DestroyCommandBuffer() {
        if (cmdBuffer) {
            vk::FreeCommandBuffers(m_device, cmdPool, 1, &cmdBuffer);
            cmdBuffer = VkCommandBuffer(0);
        }
    }

    void DestroyCommandBufferPool() {
        if (cmdPool) {
           vk::DestroyCommandPool(m_device, cmdPool, nullptr);
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
    VkDevice m_device;
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
      bufferDescriptorSet(),
      commandBuffer(),
      gfxPipeline(),
      pCurrentImage(NULL),
      lastVideoFormatUpdate((uint32_t)-1),
      currentInputBuffer(NULL)
    {
    }

    ~VulkanPerDrawContext() {
        currentInputBuffer = NULL;
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
    VulkanDescriptorSet bufferDescriptorSet;
    VulkanCommandBuffer commandBuffer;
    VulkanGraphicsPipeline gfxPipeline;
    ImageObject* pCurrentImage;
    uint32_t lastVideoFormatUpdate;
    // android::sp<PinnedBufferItem> currentInputBuffer;
    void* currentInputBuffer;
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
        m_device(),
        mNumCtxs(0),
        perDrawCtx(nullptr)
        {}


    // Create per draw contexts.
    VkResult CreatePerDrawContexts(VulkanDeviceInfo* deviceInfo,
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
    VkResult DrawFrame(VulkanDeviceInfo* deviceInfo,
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
    VkDevice m_device;
    int32_t mNumCtxs;
    VulkanPerDrawContext* perDrawCtx;

};

class VkVideoAppCtx {

    enum {MAX_NUM_BUFFER_SLOTS = 32};
public:
    bool initialized_;
    // WindowInfo window;
    VulkanDeviceInfo device_;
    bool useTestImage_;
    ImageObject testFrameImage_; // one per max queue slots
    ImageObject frameImages_[MAX_NUM_BUFFER_SLOTS];
    // VulkanSwapchainInfo swapchain_;
    VulkanRenderPass renderPass_;
    VulkanVertexBuffer vertexBuffer_;
    VulkanRenderInfo render_;

    ~VkVideoAppCtx()
    {
        if (!initialized_) {
            return;
        }

        initialized_ = false;
    }

    VkVideoAppCtx(bool testVk)
        : initialized_(false),
          // window(),
          device_(),
          useTestImage_(testVk),
          // swapchain_(),
          renderPass_(),
          vertexBuffer_(),
          render_()
    {
        CreateSamplerYcbcrConversions();
    }

    VkResult CreateSamplerYcbcrConversions();

    void ContextIsReady() {
        initialized_ = true;
    }

    bool IsContextReady() const {
        return initialized_;
    }

};

// A helper functions
// A helper function to map required memory property into a VK memory type
// memory type is an index into the array of 32 entries; or the bit index
// for the memory type ( each BIT of an 32 bit integer is a type ).
VkResult AllocateMemoryTypeFromProperties(VulkanDeviceInfo* deviceInfo,
        uint32_t typeBits,
        VkFlags requirements_mask,
        uint32_t *typeIndex);

bool MapMemoryTypeToIndex(VkPhysicalDevice gpuDevice, uint32_t typeBits,
                          VkFlags requirements_mask, uint32_t *typeIndex);

void setImageLayout(VkCommandBuffer cmdBuffer, VkImage image,
                    VkImageLayout oldImageLayout, VkImageLayout newImageLayout,
                    VkPipelineStageFlags srcStages,
                    VkPipelineStageFlags destStages, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

} // End of namespace vulkanVideoUtils

#endif // __VULKANVIDEOUTILS__
