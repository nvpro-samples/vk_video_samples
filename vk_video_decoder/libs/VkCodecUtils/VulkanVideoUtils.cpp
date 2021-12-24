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

#include <vector>
#include <vulkan_interfaces.h>
#include "pattern.h"
#include "Helpers.h"
#include <NvCodecUtils/Logger.h>
#include <shaderc/shaderc.hpp>

#include "VulkanVideoUtils.h"
#include <ycbcrvkinfo.h>

#include <time.h>

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#define exp7 10000000i64            // 1E+7     //C-file part
#define exp9 1000000000i64          // 1E+9
#define w2ux 116444736000000000i64  // 1.jan1601 to 1.jan1970
#define BILLION (1E9)
#define CLOCK_MONOTONIC 0

void unix_time(struct timespec* spec) {
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -= w2ux;
    spec->tv_sec = wintime / exp7;
    spec->tv_nsec = wintime % exp7 * 100;
}

int clock_gettime(int dummy, struct timespec* ct)
{
    static struct timespec startspec;
    static double ticks2nano;
    static __int64 startticks, tps = 0;
    __int64 tmp, curticks;
    QueryPerformanceFrequency((LARGE_INTEGER*)&tmp);  // some strange system can
    if (tps != tmp) {
        tps = tmp;  // init ~~ONCE         //possibly change freq ?
        QueryPerformanceCounter((LARGE_INTEGER*)&startticks);
        unix_time(&startspec);
        ticks2nano = (double)exp9 / tps;
    }
    QueryPerformanceCounter((LARGE_INTEGER*)&curticks);
    curticks -= startticks;
    ct->tv_sec = startspec.tv_sec + (curticks / tps);
    ct->tv_nsec = startspec.tv_nsec + (long)((double)(curticks % tps) * ticks2nano);
    if (!(ct->tv_nsec < exp9)) {
        ct->tv_sec++;
        ct->tv_nsec -= exp9;
    }
    return 0;
}
#endif

// Vulkan call wrapper
#define CALL_VK(func)                                                 \
  if (VK_SUCCESS != (func)) {                                         \
      LOG(ERROR) << "VkVideoUtils: " << "File " << __FILE__ << "line " <<  __LINE__; \
    assert(false);                                                    \
  }

// A macro to check value is VK_SUCCESS
// Used also for non-vulkan functions but return VK_SUCCESS
#define VK_CHECK(x) CALL_VK(x)

#define VK_LITERAL_TO_STRING_INTERNAL(x)    #x
#define VK_LITERAL_TO_STRING(x) VK_LITERAL_TO_STRING_INTERNAL(x)

namespace vulkanVideoUtils {

// Create vulkan device
void VulkanDeviceInfo::CreateVulkanDevice(VkApplicationInfo *appInfo)
{
    LOG(TRACE) << "VkVideoUtils: " << "Enter Function: " << __FUNCTION__ <<  "File " << __FILE__ << "line " <<  __LINE__;

    PopulateInstanceExtensions();
    printf("List of Instance Extensions:\n");
    PrintExtensions();

    std::vector<const char *> instance_extensions;
    instance_extensions.push_back("VK_KHR_surface");
#ifdef VK_KHR_android_surface
    const char* instanceExtName = "VK_KHR_android_surface";
    if(FindInstanceExtension(instanceExtName)) {
        instance_extensions.push_back(instanceExtName);
    } else {
        printf("%s instance extensions is NOT supported !!!\n", instanceExtName);
    }
#endif

    // **********************************************************
    // Create the Vulkan instance
    VkInstanceCreateInfo instanceCreateInfo = VkInstanceCreateInfo();
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pNext = nullptr;
    instanceCreateInfo.pApplicationInfo = appInfo;
    instanceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(instance_extensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = instance_extensions.data();
    instanceCreateInfo.enabledLayerCount = 0;
    instanceCreateInfo.ppEnabledLayerNames = nullptr;
    CALL_VK(vk::CreateInstance(&instanceCreateInfo, nullptr, &instance_));

    // Find one GPU to use:
    // On Android, every GPU device is equal -- supporting
    // graphics/compute/present
    // for this sample, we use the very first GPU device found on the system
    uint32_t gpuCount = 0;
    CALL_VK(vk::EnumeratePhysicalDevices(instance_,
                                       &gpuCount, nullptr));
    VkPhysicalDevice* tmpGpus = new VkPhysicalDevice[gpuCount];
    CALL_VK(vk::EnumeratePhysicalDevices(instance_,
                                       &gpuCount, tmpGpus));
    physDevice_ = tmpGpus[0];  // Pick up the first GPU Device
    delete [] tmpGpus;

    PopulateDeviceExtensions();
    printf("List of Device Extensions:\n");
    PrintExtensions(true);

    const char* deviceExtName = NULL;
    std::vector<const char *> device_extensions;
    device_extensions.push_back("VK_KHR_swapchain");
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
    deviceExtName = "VK_ANDROID_external_memory_android_hardware_buffer";
    if(FindDeviceExtension(deviceExtName)) {
        device_extensions.push_back(deviceExtName);
    } else {
        printf("%s device extensions is NOT supported !!!\n", deviceExtName);
    }
#endif // VK_ANDROID_external_memory_android_hardware_buffer
#ifdef VK_GOOGLE_display_timing
    deviceExtName = "VK_GOOGLE_display_timing";
    if(FindDeviceExtension(deviceExtName)) {
        device_extensions.push_back(deviceExtName);
    } else {
        printf("%s device extensions is NOT supported !!!\n", deviceExtName);
    }
#endif // VK_GOOGLE_display_timing
    vk::GetPhysicalDeviceMemoryProperties(
        physDevice_,
        &gpuMemoryProperties_);

    // Find a GFX queue family
    uint32_t queueFamilyCount;
    vk::GetPhysicalDeviceQueueFamilyProperties(physDevice_,
            &queueFamilyCount, nullptr);
    assert(queueFamilyCount);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
    vk::GetPhysicalDeviceQueueFamilyProperties(physDevice_,
            &queueFamilyCount,
            queueFamilyProperties.data());

    uint32_t queueFamilyIdx;
    for (queueFamilyIdx = 0; queueFamilyIdx < queueFamilyCount;
            queueFamilyIdx++) {
        if (queueFamilyProperties[queueFamilyIdx].queueFlags &
                VK_QUEUE_GRAPHICS_BIT) {
            break;
        }
    }
    assert(queueFamilyIdx < queueFamilyCount);
    queueFamilyIndex_ = queueFamilyIdx;
    // Create a logical device (vulkan device)
    float priorities[] = { 1.0f, };
    VkDeviceQueueCreateInfo queueCreateInfo = VkDeviceQueueCreateInfo();
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.pNext = nullptr;
    queueCreateInfo.flags = 0;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex_;
    queueCreateInfo.pQueuePriorities = priorities;

    VkDeviceCreateInfo deviceCreateInfo = VkDeviceCreateInfo();
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = nullptr;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledLayerCount = 0;
    deviceCreateInfo.ppEnabledLayerNames = nullptr;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = device_extensions.data();
    deviceCreateInfo.pEnabledFeatures = nullptr;
    CALL_VK(vk::CreateDevice(physDevice_, &deviceCreateInfo, nullptr, &device_));
    vk::GetDeviceQueue(device_, 0, 0, &queue_);

#ifdef VK_ANDROID_external_memory_android_hardware_buffer
    vk::GetAndroidHardwareBufferPropertiesANDROID =
            reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                    vk::GetInstanceProcAddr(instance_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    vkGetMemoryAndroidHardwareBufferANDROID =
            reinterpret_cast<PFN_vkGetMemoryAndroidHardwareBufferANDROID>(
                    vk::GetInstanceProcAddr(instance_, "vkGetMemoryAndroidHardwareBufferANDROID"));
#endif // VK_ANDROID_external_memory_android_hardware_buffer
}

void VulkanDeviceInfo::AttachVulkanDevice(VkInstance instance,
                                            VkPhysicalDevice physDevice,
                                            VkDevice device,
                                            uint32_t queueFamilyIndex,
                                            VkQueue queue,
                                            VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    instance_ = instance;
    physDevice_ = physDevice;
    device_ = device;
    queueFamilyIndex_ = queueFamilyIndex;
    queue_ = queue;
    gpuMemoryProperties_ = pMemoryProperties ? *pMemoryProperties : VkPhysicalDeviceMemoryProperties();
    attached_ = true;
}


void VulkanSwapchainInfo::CreateSwapChain(VulkanDeviceInfo* deviceInfo, VkSwapchainKHR swapchain)
{
    LOG(TRACE) << "VkVideoUtils: " << "Enter Function: " << __FUNCTION__ <<  "File " << __FILE__ << "line " <<  __LINE__;

    mInstance = deviceInfo->getInstance();
    m_device = deviceInfo->getDevice();

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    VkAndroidSurfaceCreateInfoKHR createInfo = VkAndroidSurfaceCreateInfoKHR();
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.window = platformWindow;
    CALL_VK(vk::CreateAndroidSurfaceKHR(mInstance,
                                      &createInfo, nullptr,
                                      &mSurface));
#endif // VK_USE_PLATFORM_ANDROID_KHR

    // **********************************************************
    // Get the surface capabilities because:
    //   - It contains the minimal and max length of the chain, we will need it
    //   - It's necessary to query the supported surface format (R8G8B8A8 for
    //   instance ...)
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    vk::GetPhysicalDeviceSurfaceCapabilitiesKHR(deviceInfo->physDevice_,
            mSurface,
            &surfaceCapabilities);
    // Query the list of supported surface format and choose one we like
    uint32_t formatCount = 0;
    vk::GetPhysicalDeviceSurfaceFormatsKHR(deviceInfo->physDevice_,
                                         mSurface,
                                         &formatCount, nullptr);
    VkSurfaceFormatKHR *formats = new VkSurfaceFormatKHR[formatCount];
    vk::GetPhysicalDeviceSurfaceFormatsKHR(deviceInfo->physDevice_,
                                         mSurface,
                                         &formatCount, formats);
    LOG(INFO) << "VkVideoUtils: " << "VulkanSwapchainInfo - got " << formatCount << "surface formats";

    uint32_t chosenFormat;
    for (chosenFormat = 0; chosenFormat < formatCount; chosenFormat++) {
        if (formats[chosenFormat].format == VK_FORMAT_R8G8B8A8_UNORM) break;
    }
    assert(chosenFormat < formatCount);

    mDisplaySize = surfaceCapabilities.currentExtent;
    mDisplayFormat = formats[chosenFormat].format;

#if 0
    // **********************************************************
    // Create a swap chain (here we choose the minimum available number of surface
    // in the chain)
    VkSwapchainCreateInfoKHR swapchainCreateInfo = VkSwapchainCreateInfoKHR();
    swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.pNext = nullptr;
    swapchainCreateInfo.surface = mSurface;
    swapchainCreateInfo.minImageCount = surfaceCapabilities.minImageCount;
    swapchainCreateInfo.imageFormat = formats[chosenFormat].format;
    swapchainCreateInfo.imageColorSpace = formats[chosenFormat].colorSpace;
    swapchainCreateInfo.imageExtent = surfaceCapabilities.currentExtent;
    swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.queueFamilyIndexCount = 1;
    swapchainCreateInfo.pQueueFamilyIndices = &deviceInfo->queueFamilyIndex_;
    swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    swapchainCreateInfo.clipped = VK_FALSE;
    CALL_VK(vk::CreateSwapchainKHR(m_device,
                                 &swapchainCreateInfo, nullptr,
                                 &mSwapchain));
#endif
    delete[] formats;

    mSwapchain = swapchain;

    // Get the length of the created swap chain
    CALL_VK(vk::GetSwapchainImagesKHR(
                m_device, mSwapchain,
                &mSwapchainNumBufs, nullptr));

    mDisplayImages = new VkImage[mSwapchainNumBufs];
    CALL_VK(vk::GetSwapchainImagesKHR(
                m_device, mSwapchain,
                &mSwapchainNumBufs, mDisplayImages));

    mPresentCompleteSemaphoresMem = new VkSemaphore[mSwapchainNumBufs + 1];
    mPresentCompleteSemaphores.resize(mSwapchainNumBufs, nullptr);

    for (uint32_t i = 0; i < (mSwapchainNumBufs + 1); i++) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = VkSemaphoreCreateInfo();
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphoreCreateInfo.pNext = nullptr;
        semaphoreCreateInfo.flags = 0;
        CALL_VK(vk::CreateSemaphore( m_device, &semaphoreCreateInfo, nullptr,
                    &mPresentCompleteSemaphoresMem[i]));
    }

    for (uint32_t i = 0; i < mSwapchainNumBufs; i++) {
        mPresentCompleteSemaphores[i] = &mPresentCompleteSemaphoresMem[i];
    }

    mPresentCompleteSemaphoreInFly = &mPresentCompleteSemaphoresMem[mSwapchainNumBufs];
}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
AHardwareBufferHandle ImageObject::ExportHandle()
{
    if (canBeExported) {
        AHardwareBufferHandle aHardwareBufferHandle;
        const VkMemoryGetAndroidHardwareBufferInfoANDROID getAndroidHardwareBufferInfo = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, mem};
        CALL_VK(vk::GetMemoryAndroidHardwareBufferANDROID(m_device, &getAndroidHardwareBufferInfo, &aHardwareBufferHandle));
        return aHardwareBufferHandle;
    } else {
        return NULL;
    }
}
#endif // VK_USE_PLATFORM_ANDROID_KHR

VkResult VulkanVideoBitstreamBuffer::CreateVideoBitstreamBuffer(VkPhysicalDevice gpuDevice, VkDevice device, uint32_t queueFamilyIndex,
         VkDeviceSize bufferSize, VkDeviceSize bufferOffsetAlignment,  VkDeviceSize bufferSizeAlignment,
         const unsigned char* pBitstreamData, VkDeviceSize bitstreamDataSize, VkDeviceSize dstBufferOffset)
{
    DestroyVideoBitstreamBuffer();

    m_device = device;
    m_bufferSizeAlignment = bufferSizeAlignment;
    m_bufferSize = ((bufferSize + (m_bufferSizeAlignment - 1)) & ~(m_bufferSizeAlignment - 1));
    m_bufferOffsetAlignment = bufferOffsetAlignment;

    // Create a vertex buffer
    VkBufferCreateInfo createBufferInfo = VkBufferCreateInfo();
    createBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createBufferInfo.size = m_bufferSize;
    createBufferInfo.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    createBufferInfo.flags = 0;
    createBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createBufferInfo.queueFamilyIndexCount = 1;
    createBufferInfo.pQueueFamilyIndices = &queueFamilyIndex;

    CALL_VK(vk::CreateBuffer(m_device, &createBufferInfo,
                           nullptr, &m_buffer));

    VkMemoryRequirements memReq;
    vk::GetBufferMemoryRequirements(m_device,
            m_buffer, &memReq);

    VkMemoryAllocateInfo allocInfo = VkMemoryAllocateInfo();
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.memoryTypeIndex = 0;  // Memory type assigned in the next step

    // Assign the proper memory type for that buffer
    m_bufferSize = allocInfo.allocationSize = memReq.size;
    vulkanVideoUtils::MapMemoryTypeToIndex(gpuDevice, memReq.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                         &allocInfo.memoryTypeIndex);

    // Allocate memory for the buffer
    CALL_VK(vk::AllocateMemory(m_device, &allocInfo, nullptr,
                             &m_deviceMemory));

    CALL_VK(CopyVideoBitstreamToBuffer(pBitstreamData,
                      bitstreamDataSize, dstBufferOffset = 0));

    CALL_VK(vk::BindBufferMemory(m_device,
                      m_buffer, m_deviceMemory, 0));

    return VK_SUCCESS;
}

VkResult VulkanVideoBitstreamBuffer::CopyVideoBitstreamToBuffer(const unsigned char* pBitstreamData,
        VkDeviceSize bitstreamDataSize, VkDeviceSize &dstBufferOffset) const
{
    if (pBitstreamData && bitstreamDataSize) {
        void *ptr = NULL;
        dstBufferOffset = ((dstBufferOffset + (m_bufferOffsetAlignment - 1)) & ~(m_bufferOffsetAlignment - 1));
        assert((dstBufferOffset + bitstreamDataSize) <= m_bufferSize);
        CALL_VK(vk::MapMemory(m_device, m_deviceMemory, dstBufferOffset,
                bitstreamDataSize, 0, &ptr));

        //Copy Bitstream
        // nvdec hw  requires min bitstream size to be 16 (see bug 1599347). memset padding to 0 if bitstream size less than 16
        if (bitstreamDataSize < 16) {
            memset(ptr, 0, 16);
        }

        memcpy(ptr, pBitstreamData, bitstreamDataSize);

        const VkMappedMemoryRange   range           = {
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,  // sType
            NULL,                                   // pNext
            m_deviceMemory,                         // memory
            dstBufferOffset,                        // offset
            bitstreamDataSize,                      // size
        };

        CALL_VK(vk::FlushMappedMemoryRanges(m_device, 1u, &range));

        vk::UnmapMemory(m_device, m_deviceMemory);
    }

    return VK_SUCCESS;
}

VkResult DeviceMemoryObject::AllocMemory(VkDevice device, VkMemoryRequirements* pMemoryRequirements, VkMemoryPropertyFlags requiredMemProps)
{
    if (pMemoryRequirements->memoryTypeBits == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    m_device = device;
    // Find an available memory type that satisfies the requested properties.
    uint32_t memoryTypeIndex;
    uint32_t memoryTypeBits = pMemoryRequirements->memoryTypeBits;
    for (memoryTypeIndex = 0; !(memoryTypeBits & 1); memoryTypeIndex++  ) {
        memoryTypeBits >>= 1;
    }

    VkMemoryAllocateInfo memInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,          // sType
        NULL,                                            // pNext
        pMemoryRequirements->size,                       // allocationSize
        memoryTypeIndex,                                 // memoryTypeIndex
    };

    VkResult result = vk::AllocateMemory(m_device, &memInfo, 0, &memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    return VK_SUCCESS;
}

VkResult ImageObject::CreateImage(VulkanDeviceInfo* deviceInfo,
        const VkImageCreateInfo* pImageCreateInfo,
        VkMemoryPropertyFlags requiredMemProps,
        int initWithPattern,
        VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes,
        NativeHandle& importHandle)
{
    DestroyImage();

    m_device = deviceInfo->getDevice();

    imageFormat = pImageCreateInfo->format;
    imageWidth =  pImageCreateInfo->extent.width;
    imageHeight = pImageCreateInfo->extent.height;
    imageLayout = pImageCreateInfo->initialLayout;

    const bool importMem = importHandle;
    const bool exportMem = (!importMem && (exportMemHandleTypes != 0));
    const bool external  = (importMem || exportMem);
    const bool dedicated = external;

    // Check for linear support.
    VkFormatProperties props;
    bool needBlit = true;
    vk::GetPhysicalDeviceFormatProperties(deviceInfo->physDevice_, imageFormat, &props);
    assert((props.linearTilingFeatures | props.optimalTilingFeatures) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        // linear format supporting the required texture
        needBlit = false;
    }

    const VkExternalMemoryImageCreateInfo  externalCreateInfo  = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        pImageCreateInfo->pNext,
        (VkExternalMemoryHandleTypeFlags)exportMemHandleTypes
    };

    // Allocate the linear texture so texture could be copied over
    VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
    memcpy(&imageCreateInfo, pImageCreateInfo, sizeof(imageCreateInfo));
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = external ? &externalCreateInfo : pImageCreateInfo->pNext;
    imageCreateInfo.usage = needBlit ? (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT : imageCreateInfo.usage;
    CALL_VK(vk::CreateImage(m_device, &imageCreateInfo, nullptr, &image));

    CALL_VK(AllocMemoryAndBind(deviceInfo, image, mem, requiredMemProps,
                             dedicated, exportMemHandleTypes, importHandle));

    if (importMem) {
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        bufferHandle = AHardwareBuffer_getNativeHandle(importHandle);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR)
    } else if (exportMem) {
        canBeExported = true;
        m_exportMemHandleTypes = exportMemHandleTypes;
    }

    if (!importMem && (requiredMemProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        if (initWithPattern) {
            FillImageWithPattern(initWithPattern);
        }
    }

    if (!importMem && needBlit) {
        VkResult status = StageImage(deviceInfo, imageCreateInfo.usage, requiredMemProps, needBlit);

        if (VK_SUCCESS != status) {
            return status;
        }
    }

    VkImageViewCreateInfo viewInfo = VkImageViewCreateInfo();
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = nullptr;
    viewInfo.image = VK_NULL_HANDLE;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageFormat;
    viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    viewInfo.flags = 0;
    viewInfo.image = image;
    CALL_VK(vk::CreateImageView(m_device, &viewInfo, nullptr, &view));

    return VK_SUCCESS;
}

VkResult ImageObject::AllocMemoryAndBind(VulkanDeviceInfo* deviceInfo, VkImage vkImage, VkDeviceMemory& imageDeviceMemory, VkMemoryPropertyFlags requiredMemProps,
        bool dedicated, VkExternalMemoryHandleTypeFlags exportMemHandleTypes, NativeHandle& importHandle)
{
    VkResult result;

    VkMemoryRequirements memReqs = { };
    vk::GetImageMemoryRequirements(m_device, vkImage, &memReqs);

    // Find an available memory type that satisfies the requested properties.
    uint32_t memoryTypeIndex;
    if (!MapMemoryTypeToIndex(deviceInfo->physDevice_, memReqs.memoryTypeBits, requiredMemProps, &memoryTypeIndex)) {
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    VkMemoryDedicatedAllocateInfo dedicatedAllocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,    // sType
        NULL,                                                              // pNext
        dedicated ? vkImage : VK_NULL_HANDLE,                            // image
        VK_NULL_HANDLE,                                                    // buffer
    };

    VkMemoryAllocateInfo memInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,          // sType
        NULL,                                            // pNext
        memReqs.size,                                    // allocationSize
        memoryTypeIndex,                                 // memoryTypeIndex
    };

    if (dedicated) {
        memInfo.pNext = &dedicatedAllocInfo;
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkExportMemoryAllocateInfo exportInfo  = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        NULL,
        exportMemHandleTypes
    };

    if (newAndroidHardwareBuffer) {
        VkAndroidHardwareBufferPropertiesANDROID androidHardwareBufferProperties = VkAndroidHardwareBufferPropertiesANDROID();
        androidHardwareBufferProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        result = vk::GetAndroidHardwareBufferPropertiesANDROID(m_device, newAndroidHardwareBuffer, &androidHardwareBufferProperties);
        if (result != VK_SUCCESS) {
            return result;
        }
        memInfo.allocationSize = androidHardwareBufferProperties.allocationSize;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo = {
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        NULL,
        newAndroidHardwareBuffer
    };

    if (newAndroidHardwareBuffer) {
        importInfo.pNext = memInfo.pNext;
        memInfo.pNext = &importInfo;
    } else if (exportMemHandleTypes) {
        exportInfo.pNext = memInfo.pNext;
        memInfo.pNext = &exportInfo;
    }
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR)

    result = vk::AllocateMemory(m_device, &memInfo, 0, &imageDeviceMemory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vk::BindImageMemory(m_device, vkImage, imageDeviceMemory, 0);
    if (result != VK_SUCCESS) {
        vk::FreeMemory(m_device, imageDeviceMemory, 0);
        imageDeviceMemory = 0;
        return result;
    }

    return VK_SUCCESS;
}


int32_t ImageObject::GetImageSubresourceAndLayout(VkSubresourceLayout layouts[3]) const
{
    int numPlanes = 0;
    const VkMpFormatInfo *mpInfo =  YcbcrVkFormatInfo(imageFormat);
    VkImageSubresource subResource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    if (mpInfo) {
        switch (mpInfo->planesLayout.layout) {
        case YCBCR_SINGLE_PLANE_UNNORMALIZED:
        case YCBCR_SINGLE_PLANE_INTERLEAVED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            numPlanes = 1;
            break;
        case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[1]);
            numPlanes = 2;
            break;
        case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
        case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
        case YCBCR_PLANAR_STRIDE_PADDED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[1]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[2]);
            numPlanes = 3;
            break;
        default:
            assert(0);
        }
    } else {
        subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
        numPlanes = 1;
    }

    return numPlanes;
}

VkResult ImageObject::GetMemoryFd(int* pFd) const
{
    const VkMemoryGetFdInfoKHR getFdInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                             NULL,
                                             mem, m_exportMemHandleTypes };

    return vk::GetMemoryFdKHR(m_device, &getFdInfo, pFd);
}

VkResult ImageObject::FillImageWithPattern(int pattern)
{
    const VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };

    VkSubresourceLayout layout;
    void *data;

    VkMemoryRequirements mem_reqs;
    vk::GetImageMemoryRequirements(m_device, image, &mem_reqs);
    VkDeviceSize allocationSize = mem_reqs.size;

    vk::GetImageSubresourceLayout(m_device, image, &subres, &layout);

    CALL_VK(vk::MapMemory(m_device, mem, 0, allocationSize, 0, &data));


    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(imageFormat);
    if (mpInfo) {
        ImageData imageData =
            // 8/16-bit format and data. The format fields are updated based on the test format input.
            { imageFormat,          (uint32_t)imageWidth,  (uint32_t)imageHeight, (ColorPattern)pattern, {0xFF, 0x00, 0x00, 0xFF},  NULL };

        const VkSamplerYcbcrConversionCreateInfo ycbcrConversionInfo = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL,
                imageFormat, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        VK_CHROMA_LOCATION_MIDPOINT, VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, false
                                                                              };

        VkFillYuv vkFillYuv;
        vkFillYuv.fillVkImage(m_device, image, &imageData, mem, &ycbcrConversionInfo);
    } else {
        generateColorPatternRgba8888((ColorPattern)pattern, (uint8_t *)data,
                                 imageWidth, imageHeight,
                                 (uint32_t)layout.rowPitch);
    }

    vk::UnmapMemory(m_device, mem);

    return VK_SUCCESS;
}

// Initialize the texture data, either directly into the texture itself
// or into buffer memory.
VkResult ImageObject::CopyYuvToVkImage(uint32_t numPlanes, const uint8_t* yuvPlaneData[3], const VkSubresourceLayout yuvPlaneLayouts[3])
{
    uint8_t *ptr = NULL;
    VkResult result;

    VkImageSubresource subResource = {};
    VkSubresourceLayout layouts[3];
    VkDeviceSize size   = 0;

    int cbimageHeight = imageHeight;

    // Clean it
    memset(layouts, 0x00, sizeof(layouts));

    const VkMpFormatInfo *mpInfo =  YcbcrVkFormatInfo(imageFormat);
    bool isUnnormalizedRgba = false;
    if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
        isUnnormalizedRgba = true;
    }

    if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY) {
        cbimageHeight /= 2;
    }

    if (mpInfo && !isUnnormalizedRgba) {
        VkMemoryRequirements memReqs = { };
        vk::GetImageMemoryRequirements(m_device, image, &memReqs);
        size      = memReqs.size;
        // alignment = memReqs.alignment;
        switch (mpInfo->planesLayout.layout) {
        case YCBCR_SINGLE_PLANE_UNNORMALIZED:
        case YCBCR_SINGLE_PLANE_INTERLEAVED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            break;
        case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[1]);
            break;
        case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
        case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
        case YCBCR_PLANAR_STRIDE_PADDED:
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[1]);
            subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
            vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[2]);
            break;
        default:
            assert(0);
        }

    } else {

        vk::GetImageSubresourceLayout(m_device, image, &subResource, &layouts[0]);
        size = layouts[0].size;
    }

    vk::MapMemory(m_device, mem, 0, size, 0, (void **)&ptr);

    for (uint32_t plane = 0; plane < numPlanes; plane++) {
        int copyHeight = plane ? cbimageHeight : imageHeight;
        uint8_t* pDst = ptr + layouts[plane].offset;
        const uint8_t* pSrc = yuvPlaneData[plane] + yuvPlaneLayouts[plane].offset;
        for (int height = 0; height < copyHeight; height++) {
            memcpy(pDst, pSrc, layouts[plane].rowPitch);
            pDst += layouts[plane].rowPitch;
            pSrc += yuvPlaneLayouts[plane].rowPitch;
        }
    }

    const VkMappedMemoryRange   range           = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,  // sType
        NULL,                                   // pNext
        mem,                                    // memory
        0,                                      // offset
        size,                                   // size
    };

    result = vk::FlushMappedMemoryRanges(m_device, 1u, &range);

    vk::UnmapMemory(m_device, mem);

    return result;
}

VkResult ImageObject::StageImage(VulkanDeviceInfo* deviceInfo, VkImageUsageFlags usage, VkMemoryPropertyFlags requiredMemProps, bool needBlit)
{
    if (!(usage | requiredMemProps)) {
        LOG(ERROR) << "VkVideoUtils: " << "image No usage and required_pros" << "File:" << __FILE__ << "line " <<  __LINE__;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkCommandPoolCreateInfo cmdPoolCreateInfo = VkCommandPoolCreateInfo();
    cmdPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolCreateInfo.pNext = nullptr;
    cmdPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmdPoolCreateInfo.queueFamilyIndex = deviceInfo->queueFamilyIndex_;

    VkCommandPool cmdPool;
    CALL_VK(vk::CreateCommandPool(m_device,
                                &cmdPoolCreateInfo, nullptr, &cmdPool));

    VkCommandBuffer gfxCmd;
    VkCommandBufferAllocateInfo cmd = VkCommandBufferAllocateInfo();
    cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd.pNext = nullptr;
    cmd.commandPool = cmdPool;
    cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd.commandBufferCount = 1;

    CALL_VK(vk::AllocateCommandBuffers(m_device, &cmd, &gfxCmd));

    VkCommandBufferBeginInfo cmd_buf_info = VkCommandBufferBeginInfo();
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = nullptr;
    cmd_buf_info.flags = 0;
    cmd_buf_info.pInheritanceInfo = nullptr;
    CALL_VK(vk::BeginCommandBuffer(gfxCmd, &cmd_buf_info));

    // If linear is supported, we are done
    VkImage stageImage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    if (!needBlit) {
        setImageLayout(gfxCmd, image, VK_IMAGE_LAYOUT_PREINITIALIZED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    } else {
        // save current image and mem as staging image and memory
        stageImage = image;
        stageMem = mem;
        image = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;

        // Create a tile texture to blit into
        VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.pNext = nullptr;
        imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        imageCreateInfo.format = imageFormat;
        imageCreateInfo.extent = {
                static_cast<uint32_t>(imageWidth),
                static_cast<uint32_t>(imageHeight), 1};
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.queueFamilyIndexCount = 1;
        imageCreateInfo.pQueueFamilyIndices = &deviceInfo->queueFamilyIndex_;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateInfo.flags = 0;
        CALL_VK(vk::CreateImage(m_device, &imageCreateInfo,
                              nullptr, &image));

        VkMemoryRequirements mem_reqs;
        vk::GetImageMemoryRequirements(m_device, image,
                                     &mem_reqs);

        VkMemoryAllocateInfo mem_alloc = VkMemoryAllocateInfo();
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mem_alloc.pNext = nullptr;
        mem_alloc.memoryTypeIndex = 0;
        mem_alloc.allocationSize = mem_reqs.size;
        VK_CHECK(AllocateMemoryTypeFromProperties(
                deviceInfo, mem_reqs.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_alloc.memoryTypeIndex));
        CALL_VK(vk::AllocateMemory(m_device, &mem_alloc,
                                 nullptr, &mem));
        CALL_VK(vk::BindImageMemory(m_device, image,
                                  mem, 0));

        // transitions image out of UNDEFINED type
        setImageLayout(gfxCmd, stageImage, VK_IMAGE_LAYOUT_PREINITIALIZED,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        setImageLayout(gfxCmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageCopy bltInfo = VkImageCopy();
        bltInfo.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bltInfo.srcSubresource.mipLevel = 0;
        bltInfo.srcSubresource.baseArrayLayer = 0;
        bltInfo.srcSubresource.layerCount = 1;
        bltInfo.srcOffset.x = 0;
        bltInfo.srcOffset.y = 0;
        bltInfo.srcOffset.z = 0;
        bltInfo.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bltInfo.dstSubresource.mipLevel = 0;
        bltInfo.dstSubresource.baseArrayLayer = 0;
        bltInfo.dstSubresource.layerCount = 1;
        bltInfo.dstOffset.x = 0;
        bltInfo.dstOffset.y = 0;
        bltInfo.dstOffset.z = 0;
        bltInfo.extent.width = (uint32_t)imageWidth;
        bltInfo.extent.height = (uint32_t)imageHeight;
        bltInfo.extent.depth = 1;
        vk::CmdCopyImage(gfxCmd, stageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &bltInfo);

        setImageLayout(gfxCmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    CALL_VK(vk::EndCommandBuffer(gfxCmd));
    VkFenceCreateInfo fenceInfo = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        nullptr,
        0
    };
    VkFence fence;
    CALL_VK(vk::CreateFence(m_device, &fenceInfo, nullptr,
                          &fence));

    VkSubmitInfo submitInfo = VkSubmitInfo();
    submitInfo.pNext = nullptr;
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.pWaitDstStageMask = nullptr;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &gfxCmd;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = nullptr;
    CALL_VK(vk::QueueSubmit(deviceInfo->queue_, 1, &submitInfo, fence) !=
            VK_SUCCESS);
    CALL_VK(vk::WaitForFences(m_device, 1, &fence, VK_TRUE,
                            100000000) != VK_SUCCESS);
    vk::DestroyFence(m_device, fence, nullptr);

    vk::FreeCommandBuffers(m_device, cmdPool, 1, &gfxCmd);
    vk::DestroyCommandPool(m_device, cmdPool, nullptr);
    if (stageImage != VK_NULL_HANDLE) {
        vk::DestroyImage(m_device, stageImage, nullptr);
        vk::FreeMemory(m_device, stageMem, nullptr);
    }
    return VK_SUCCESS;
}

VkResult VulkanFrameBuffer::CreateFrameBuffer(VkDevice device, VkSwapchainKHR swapchain,
        const VkExtent2D* pExtent2D,const VkSurfaceFormatKHR* pSurfaceFormat, VkImage fbImage,
        VkRenderPass renderPass, VkImageView depthView)
{
    DestroyFrameBuffer();
    m_device = device;

    mFbImage = fbImage;

    VkImageViewCreateInfo viewCreateInfo = VkImageViewCreateInfo();
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.pNext = nullptr;
    viewCreateInfo.image = fbImage;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = pSurfaceFormat->format;
    viewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_R;
    viewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_G;
    viewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_B;
    viewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_A;
    viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCreateInfo.subresourceRange.baseMipLevel = 0;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.baseArrayLayer = 0;
    viewCreateInfo.subresourceRange.layerCount = 1;
    viewCreateInfo.flags = 0;
    CALL_VK(vk::CreateImageView(m_device, &viewCreateInfo, nullptr, &mImageView));

    VkImageView attachments[2] = { mImageView, depthView };
    VkFramebufferCreateInfo fbCreateInfo = VkFramebufferCreateInfo();
    fbCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCreateInfo.pNext = nullptr;
    fbCreateInfo.renderPass = renderPass;
    fbCreateInfo.layers = 1;
    fbCreateInfo.attachmentCount = 1;  // 2 if using depth
    fbCreateInfo.pAttachments = attachments;
    fbCreateInfo.width = pExtent2D->width;
    fbCreateInfo.height = pExtent2D->height;
    fbCreateInfo.attachmentCount = (depthView == VK_NULL_HANDLE ? 1 : 2);
    CALL_VK(vk::CreateFramebuffer(m_device, &fbCreateInfo, nullptr, &mFramebuffer));

    return VK_SUCCESS;
}

VkResult VulkanSyncPrimitives::CreateSyncPrimitives(VkDevice device)
{
    DestroySyncPrimitives();
    m_device = device;

    // Create a fence to be able, in the main loop, to wait for the
    // draw command(s) to finish before swapping the framebuffers
    VkFenceCreateInfo fenceCreateInfo = VkFenceCreateInfo();
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.pNext = nullptr;
      // Create in signaled state so we don't wait on first render of each
      // command buffer
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CALL_VK(vk::CreateFence(m_device, &fenceCreateInfo, nullptr, &mFence));

    // We need to create a semaphore to be able to wait on, in the main loop, for
    // the framebuffer to be available before drawing.
    VkSemaphoreCreateInfo semaphoreCreateInfo = VkSemaphoreCreateInfo();
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreCreateInfo.pNext = nullptr;
    semaphoreCreateInfo.flags = 0;
    CALL_VK(vk::CreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &mRenderCompleteSemaphore));

    return VK_SUCCESS;
}

bool VulkanSamplerYcbcrConversion::SamplerRequiresUpdate(const VkSamplerCreateInfo* pSamplerCreateInfo,
            const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{
    if (pSamplerCreateInfo) {
        if (memcmp(&mSamplerInfo, pSamplerCreateInfo, sizeof(mSamplerInfo))) {
            return true;
        }
    }

    if (pSamplerYcbcrConversionCreateInfo) {
        if (memcmp(&mSamplerYcbcrConversionCreateInfo, pSamplerYcbcrConversionCreateInfo, sizeof(mSamplerYcbcrConversionCreateInfo))) {
            return true;
        }
    }

    return false;
}


VkResult VulkanSamplerYcbcrConversion::CreateVulkanSampler(VkDevice device,
        const VkSamplerCreateInfo* pSamplerCreateInfo,
        const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{
    m_device = device;

    DestroyVulkanSampler();

    VkSamplerYcbcrConversionInfo* pSamplerColorConversion = NULL;
    VkSamplerYcbcrConversionInfo samplerColorConversion = VkSamplerYcbcrConversionInfo();
    const VkMpFormatInfo* mpInfo = pSamplerYcbcrConversionCreateInfo ? YcbcrVkFormatInfo(pSamplerYcbcrConversionCreateInfo->format) : nullptr;
    if (mpInfo) {

        memcpy(&mSamplerYcbcrConversionCreateInfo, pSamplerYcbcrConversionCreateInfo, sizeof(mSamplerYcbcrConversionCreateInfo));
        CALL_VK(vk::CreateSamplerYcbcrConversion(m_device, &mSamplerYcbcrConversionCreateInfo, NULL, &mSamplerYcbcrConversion));

        pSamplerColorConversion = &samplerColorConversion;
        pSamplerColorConversion->sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        pSamplerColorConversion->conversion = mSamplerYcbcrConversion;
    }

    const VkSamplerCreateInfo defaultSamplerInfo = {
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR,  VK_FILTER_LINEAR,  VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor                   unnormalizedCoordinates
                 0.0,          false,            0.00,         false,       VK_COMPARE_OP_NEVER,   0.0,   16.0,    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,        false };

    if (pSamplerCreateInfo) {
        memcpy(&mSamplerInfo, pSamplerCreateInfo, sizeof(mSamplerInfo));
    } else {
        memcpy(&mSamplerInfo, &defaultSamplerInfo, sizeof(defaultSamplerInfo));
    }
    mSamplerInfo.pNext = pSamplerColorConversion;
    CALL_VK(vk::CreateSampler(m_device, &mSamplerInfo, nullptr, &sampler));

    mSamplerInfo.pNext = 0;

    return VK_SUCCESS;
}

VkResult VulkanRenderPass::CreateRenderPass(VulkanDeviceInfo* deviceInfo, VkFormat displayImageFormat)
{
    DestroyRenderPass();

    m_device = deviceInfo->getDevice();
    // -----------------------------------------------------------------
    // Create render pass
    VkAttachmentDescription attachmentDescriptions = VkAttachmentDescription();
    attachmentDescriptions.format = displayImageFormat;
    attachmentDescriptions.samples = VK_SAMPLE_COUNT_1_BIT;
    attachmentDescriptions.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachmentDescriptions.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachmentDescriptions.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachmentDescriptions.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachmentDescriptions.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachmentDescriptions.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colourReference = VkAttachmentReference();
    colourReference.attachment = 0;
    colourReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpassDescription = VkSubpassDescription();
    subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDescription.flags = 0;
    subpassDescription.inputAttachmentCount = 0;
    subpassDescription.pInputAttachments = nullptr;
    subpassDescription.colorAttachmentCount = 1;
    subpassDescription.pColorAttachments = &colourReference;
    subpassDescription.pResolveAttachments = nullptr;
    subpassDescription.pDepthStencilAttachment = nullptr;
    subpassDescription.preserveAttachmentCount = 0;
    subpassDescription.pPreserveAttachments = nullptr;

    VkSubpassDependency dependencies[2];
    // First dependency at the start of the renderpass
    // Does the transition from final to initial layout
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;                               // Producer of the dependency
    dependencies[0].dstSubpass = 0;                                                 // Consumer is our single subpass that will wait for the execution dependency
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Second dependency at the end the renderpass
    // Does the transition from the initial to the final layout
    dependencies[1].srcSubpass = 0;                                                 // Producer of the dependency is our single subpass
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;                               // Consumer are all commands outside of the renderpass
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassCreateInfo = VkRenderPassCreateInfo();
    renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassCreateInfo.pNext = nullptr;
    renderPassCreateInfo.attachmentCount = 1;
    renderPassCreateInfo.pAttachments = &attachmentDescriptions;
    renderPassCreateInfo.subpassCount = 1;
    renderPassCreateInfo.pSubpasses = &subpassDescription;
    renderPassCreateInfo.dependencyCount = sizeof(dependencies)/sizeof(dependencies[0]);
    renderPassCreateInfo.pDependencies = dependencies;

   CALL_VK(vk::CreateRenderPass(m_device, &renderPassCreateInfo, nullptr, &renderPass));

   return VK_SUCCESS;

}

VkResult VulkanVertexBuffer::CreateVertexBuffer(VulkanDeviceInfo* deviceInfo,  const float* pVertexData, VkDeviceSize vertexDataSize, uint32_t _numVertices)
{
    DestroyVertexBuffer();

    m_device = deviceInfo->getDevice();
    uint32_t queueFamilyIndex = deviceInfo->queueFamilyIndex_;
    VkPhysicalDevice gpuDevice = deviceInfo->physDevice_;

    // Create a vertex buffer
    VkBufferCreateInfo createBufferInfo = VkBufferCreateInfo();
    createBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createBufferInfo.size = vertexDataSize;
    createBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    createBufferInfo.flags = 0;
    createBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createBufferInfo.queueFamilyIndexCount = 1;
    createBufferInfo.pQueueFamilyIndices = &queueFamilyIndex;

    CALL_VK(vk::CreateBuffer(m_device, &createBufferInfo,
                           nullptr, &vertexBuffer));

    VkMemoryRequirements memReq;
    vk::GetBufferMemoryRequirements(m_device,
                                  vertexBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo = VkMemoryAllocateInfo();
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = vertexDataSize;
    allocInfo.memoryTypeIndex = 0;  // Memory type assigned in the next step

    // Assign the proper memory type for that buffer
    allocInfo.allocationSize = memReq.size;
    MapMemoryTypeToIndex(gpuDevice, memReq.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                         &allocInfo.memoryTypeIndex);

    // Allocate memory for the buffer
    CALL_VK(vk::AllocateMemory(m_device, &allocInfo, nullptr,
                             &deviceMemory));

    void *data;
    CALL_VK(vk::MapMemory(m_device, deviceMemory, 0,
            vertexDataSize, 0, &data));
    memcpy(data, pVertexData, vertexDataSize);
    vk::UnmapMemory(m_device, deviceMemory);

    CALL_VK(vk::BindBufferMemory(m_device,
                               vertexBuffer, deviceMemory,
                               0));

    numVertices = _numVertices;
    return VK_SUCCESS;
}

VkResult VulkanDescriptorSet::WriteDescriptorSet(VkSampler sampler, VkImageView imageView, VkImageLayout imageLayout)
{
    VkDescriptorImageInfo imageDsts;
    imageDsts.sampler = sampler;
    imageDsts.imageView = imageView;
    imageDsts.imageLayout = imageLayout;

    VkWriteDescriptorSet writeDst = VkWriteDescriptorSet();
    writeDst.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDst.pNext = nullptr;
    writeDst.dstSet = descSet;
    writeDst.dstBinding = 0;
    writeDst.dstArrayElement = 0;
    writeDst.descriptorCount = 1;
    writeDst.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writeDst.pImageInfo = &imageDsts;
    writeDst.pBufferInfo = nullptr;
    writeDst.pTexelBufferView = nullptr;
    vk::UpdateDescriptorSets(m_device, 1, &writeDst, 0, nullptr);

    return VK_SUCCESS;
}

VkResult VulkanDescriptorSet::CreateFragmentShaderOutput(VkDescriptorType outMode, uint32_t outSet, uint32_t outBinding, uint32_t outArrayIndex, std::stringstream& imageFss)
{
    switch (outMode) {
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        imageFss <<
               "void main()\n"
               "{\n"
               "    oFrag = texture(tex" << outSet << outBinding << "[" << outArrayIndex << "], vTexCoord);\n"
               "}\n";
        break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        imageFss <<
               "void main()\n"
               "{\n"
               "    oFrag = ubo" << outSet << outBinding << "[" << outArrayIndex << "].color;\n"
               "}\n";
        break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        imageFss <<
               "void main()\n"
               "{\n"
               "    oFrag = ssbo" << outSet << outBinding << "[" << outArrayIndex << "].color;\n"
               "}\n";
        break;
    default:
        assert(0);
        break;
    }

    // printf("\nFragment shader output code:\n %s", imageFss.str().c_str());

    return VK_SUCCESS;
}

VkResult VulkanDescriptorSet::CreateFragmentShaderLayouts(const uint32_t* setIds, uint32_t numSets, std::stringstream& imageFss)
{
    const VkDescriptorSetLayoutCreateInfo * pDescriptorSetEntries = &descriptorSetLayoutCreateInfo;

    imageFss <<
           "#version 450 core\n"
           "layout(location = 0) in vec2 vTexCoord;\n"
           "layout(location = 0) out vec4 oFrag;\n";

    for (uint32_t setIndex = 0; setIndex < numSets; setIndex++) {
        const VkDescriptorSetLayoutCreateInfo * pDescriptorSetEntry = &pDescriptorSetEntries[setIndex];
        uint32_t setId = setIds[setIndex];

        for (uint32_t bindingIndex = 0; bindingIndex < pDescriptorSetEntry->bindingCount; bindingIndex++) {

            const VkDescriptorSetLayoutBinding* pBinding = &pDescriptorSetEntry->pBindings[bindingIndex];

            switch (pBinding->descriptorType) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                imageFss <<
                       "layout(set = " << setId << ", binding = " << pBinding->binding << ") uniform sampler2D tex" <<
                       setId << pBinding->binding << "[" << pBinding->descriptorCount << "];\n";
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                imageFss <<
                       "layout(std140, set = " << setId << ", binding = " << pBinding->binding << ") uniform ubodef" <<
                       setId << pBinding->binding << " { vec4 color; } ubo" <<
                       setId << pBinding->binding << "[" << pBinding->descriptorCount << "];\n";
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                imageFss <<
                       "layout(std140, set = " << setId << ", binding = " << pBinding->binding << ") buffer ssbodef" <<
                       setId << pBinding->binding << " { vec4 color; } ssbo" <<
                       setId << pBinding->binding << "[" << pBinding->descriptorCount << "];\n";
                break;
            default:
                assert(0);
                break;
            }
        }
    }
    // printf("\nFragment shader layout code:\n %s", imageFss.str().c_str());

    return VK_SUCCESS;
}

// initialize descriptor set
VkResult VulkanDescriptorSet::CreateDescriptorSet(VkDevice device,
        uint32_t descriptorCount, const VkSampler* pImmutableSamplers)
{
    m_device = device;

    DestroyDescriptorSets();
    DestroyPipelineLayout();
    DestroyDescriptorSetLayout();

    descriptorSetLayoutBinding.binding = 0;
    descriptorSetLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorSetLayoutBinding.descriptorCount = descriptorCount; // Only one image at the time.
    descriptorSetLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorSetLayoutBinding.pImmutableSamplers = pImmutableSamplers; // we must use ImmutableSamplers here because of a potential of using ycbcr.

    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.pNext = nullptr;
    descriptorSetLayoutCreateInfo.bindingCount = descriptorCount;
    descriptorSetLayoutCreateInfo.pBindings = &descriptorSetLayoutBinding;
    CALL_VK(vk::CreateDescriptorSetLayout(m_device,
                                        &descriptorSetLayoutCreateInfo, nullptr,
                                        &dscLayout));

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = VkPipelineLayoutCreateInfo();
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.pNext = nullptr;
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &dscLayout;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

    //setup push constants
    VkPushConstantRange push_constant;
    //this push constant range starts at the beginning
    push_constant.offset = 0;
    //this push constant range takes up the size of a MeshPushConstants struct
    push_constant.size = sizeof(TransformPushConstants);
    //this push constant range is accessible only in the vertex shader
    push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pipelineLayoutCreateInfo.pPushConstantRanges = &push_constant;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;

    CALL_VK(vk::CreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

    if (descPool == VkDescriptorPool(0)) {

        VkDescriptorPoolSize type_count = VkDescriptorPoolSize();
        type_count.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        type_count.descriptorCount = descriptorCount;

        VkDescriptorPoolCreateInfo descriptor_pool = VkDescriptorPoolCreateInfo();
        descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool.pNext = nullptr;
        descriptor_pool.maxSets = 1;
        descriptor_pool.poolSizeCount = descriptorCount;
        descriptor_pool.pPoolSizes = &type_count;
        CALL_VK(vk::CreateDescriptorPool(m_device, &descriptor_pool, nullptr, &descPool));
    }

    VkDescriptorSetAllocateInfo alloc_info = VkDescriptorSetAllocateInfo();
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = nullptr;
    alloc_info.descriptorPool = descPool;
    alloc_info.descriptorSetCount = descriptorCount;
    alloc_info.pSetLayouts = &dscLayout;
    CALL_VK(vk::AllocateDescriptorSets(m_device, &alloc_info, &descSet));

    return VK_SUCCESS;
}

// Create Graphics Pipeline
VkResult VulkanGraphicsPipeline::CreateGraphicsPipeline(VkDevice device, VkViewport* pViewport, VkRect2D* pScissor,
        VkRenderPass renderPass, VulkanDescriptorSet* pBufferDescriptorSets)
{
    m_device = device;

    if (cache == VkPipelineCache(0)) {
        // Create the pipeline cache
        VkPipelineCacheCreateInfo pipelineCacheInfo = VkPipelineCacheCreateInfo();
        pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        pipelineCacheInfo.pNext = nullptr;
        pipelineCacheInfo.initialDataSize = 0;
        pipelineCacheInfo.pInitialData = nullptr;
        pipelineCacheInfo.flags = 0;  // reserved, must be 0
        CALL_VK(vk::CreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &cache));
    }

    // No dynamic state in that tutorial
    VkPipelineDynamicStateCreateInfo dynamicStateInfo = VkPipelineDynamicStateCreateInfo();
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.pNext = nullptr;
    dynamicStateInfo.dynamicStateCount = 0;
    dynamicStateInfo.pDynamicStates = nullptr;

    // See https://vkguide.dev/docs/chapter-3/push_constants/
    static char const vss[] =
        "#version 450 core\n"
        "layout(location = 0) in vec2 aVertex;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "layout(location = 0) out vec2 vTexCoord;\n"
        "\n"
        "layout( push_constant ) uniform constants\n"
        "{\n"
        "    mat4 posMatrix;\n"
        "    mat2 texMatrix;\n"
        "} transformPushConstants;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    vTexCoord = transformPushConstants.texMatrix * aTexCoord;\n"
        "    gl_Position = vec4(aVertex, 0, 1);\n"
        "}\n"
        ;

    std::stringstream imageFss;
    const uint32_t setIds[] = {0};
    const uint32_t setIndex = 0;
    const uint32_t bindingIndex = 0;
    const uint32_t arrayIndex = 0;
    pBufferDescriptorSets->CreateFragmentShaderLayouts(setIds, sizeof(setIds)/sizeof(setIds[0]), imageFss);
    pBufferDescriptorSets->CreateFragmentShaderOutput(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setIds[setIndex], bindingIndex, arrayIndex, imageFss);

    printf("\nVertex shader output code:\n %s", vss);
    printf("\nFragment shader output code:\n %s", imageFss.str().c_str());

    const bool loadShadersFromFile = false;
    if (loadShadersFromFile) {

        DestroyVertexShaderModule();
        mVulkanShaderCompiler.BuildShaderFromFile("/sdcard/vulkan_video_demo/shaders/tri.vert",
                            VK_SHADER_STAGE_VERTEX_BIT,
                            m_device, &mVertexShaderCache);

        DestroyFragmentShaderModule();
        mVulkanShaderCompiler.BuildShaderFromFile("/sdcard/vulkan_video_demo/shaders/tri.frag",
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                            m_device, &mFragmentShaderCache);
    } else {

        if (mVertexShaderCache == VkShaderModule(0)) {
            mVulkanShaderCompiler.BuildGlslShader(vss, strlen(vss),
                    VK_SHADER_STAGE_VERTEX_BIT,
                    m_device, &mVertexShaderCache);
        }

        if (mFssCache.str() != imageFss.str()) {
            DestroyFragmentShaderModule();
            mVulkanShaderCompiler.BuildGlslShader(imageFss.str().c_str(), strlen(imageFss.str().c_str()),
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                m_device, &mFragmentShaderCache);

            mFssCache.swap(imageFss);
            printf("\nFragment shader cache output code:\n %s", mFssCache.str().c_str());
        }
    }

    // Specify vertex and fragment shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2];
    shaderStages[1].sType = shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
    shaderStages[1].pNext = shaderStages[0].pNext = nullptr;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[1].pName = shaderStages[0].pName = "main";
    shaderStages[0].module = mVertexShaderCache;
    shaderStages[0].pSpecializationInfo = nullptr;
    shaderStages[0].flags = 0;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = mFragmentShaderCache;
    shaderStages[1].pSpecializationInfo = nullptr;
    shaderStages[1].flags = 0;

    // Specify viewport info
    VkPipelineViewportStateCreateInfo viewportInfo = VkPipelineViewportStateCreateInfo();
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.pNext = nullptr;
    viewportInfo.viewportCount = 1;
    viewportInfo.pViewports = pViewport;
    viewportInfo.scissorCount = 1;
    viewportInfo.pScissors = pScissor;

    // Specify multisample info
    VkSampleMask sampleMask = ~0u;
    VkPipelineMultisampleStateCreateInfo multisampleInfo = VkPipelineMultisampleStateCreateInfo();
    multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleInfo.pNext = nullptr;
    multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampleInfo.sampleShadingEnable = VK_FALSE;
    multisampleInfo.minSampleShading = 0;
    multisampleInfo.pSampleMask = &sampleMask;
    multisampleInfo.alphaToCoverageEnable = VK_FALSE;
    multisampleInfo.alphaToOneEnable = VK_FALSE;

    // Specify color blend state
    VkPipelineColorBlendAttachmentState attachmentStates = VkPipelineColorBlendAttachmentState();
    attachmentStates.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    attachmentStates.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo = VkPipelineColorBlendStateCreateInfo();
    colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.pNext = nullptr;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.logicOp = VK_LOGIC_OP_COPY;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &attachmentStates;
    colorBlendInfo.flags = 0;

    // Specify rasterizer info
    VkPipelineRasterizationStateCreateInfo rasterInfo = VkPipelineRasterizationStateCreateInfo();
    rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterInfo.pNext = nullptr;
    rasterInfo.depthClampEnable = VK_FALSE;
    rasterInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rasterInfo.cullMode = VK_CULL_MODE_NONE;
    rasterInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterInfo.depthBiasEnable = VK_FALSE;
    rasterInfo.lineWidth = 1;

    // Specify input assembler state
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo = VkPipelineInputAssemblyStateCreateInfo();
    inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.pNext = nullptr;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // Specify vertex input state
    VkVertexInputBindingDescription vertex_input_bindings = VkVertexInputBindingDescription();
    vertex_input_bindings.binding = 0;
    vertex_input_bindings.stride = sizeof(Vertex);
    vertex_input_bindings.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_input_attributes[2];
    vertex_input_attributes[0].location = 0;
    vertex_input_attributes[0].binding = 0;
    vertex_input_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_input_attributes[0].offset = offsetof(Vertex, position);
    vertex_input_attributes[1].location = 1;
    vertex_input_attributes[1].binding = 0;
    vertex_input_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_input_attributes[1].offset = offsetof(Vertex, texCoord);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = VkPipelineVertexInputStateCreateInfo();
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.pNext = nullptr;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &vertex_input_bindings;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = vertex_input_attributes;

    // Create the pipeline
    VkGraphicsPipelineCreateInfo pipelineCreateInfo = VkGraphicsPipelineCreateInfo();
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.pNext = nullptr;
    pipelineCreateInfo.flags = 0;
    pipelineCreateInfo.stageCount = 2;
    pipelineCreateInfo.pStages = shaderStages;
    pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineCreateInfo.pTessellationState = nullptr;
    pipelineCreateInfo.pViewportState = &viewportInfo;
    pipelineCreateInfo.pRasterizationState = &rasterInfo;
    pipelineCreateInfo.pMultisampleState = &multisampleInfo;
    pipelineCreateInfo.pDepthStencilState = nullptr;
    pipelineCreateInfo.pColorBlendState = &colorBlendInfo;
    pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
    pipelineCreateInfo.layout = pBufferDescriptorSets->getPipelineLayout();
    pipelineCreateInfo.renderPass = renderPass;
    pipelineCreateInfo.subpass = 0;
    pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineCreateInfo.basePipelineIndex = 0;

    // Make sure we destroy the existing pipeline, if it were to exist.
    DestroyPipeline();
    VkResult pipelineResult = vk::CreateGraphicsPipelines(m_device, cache, 1,
                                  &pipelineCreateInfo, nullptr, &pipeline);

    return pipelineResult;
}

VkResult VulkanCommandBuffer::CreateCommandBuffer(VkRenderPass renderPass, const ImageObject* inputImageToDrawFrom,
        int32_t displayWidth, int32_t displayHeight,
        VkImage displayImage, VkFramebuffer framebuffer, VkRect2D* pRenderArea,
        VkPipeline pipeline, VkPipelineLayout pipelineLayout, const VkDescriptorSet* pDescriptorSet,
        VulkanVertexBuffer* pVertexBuffer)
{
    // 1 command buffer draw in 1 framebuffer
    if (cmdBuffer == VkCommandBuffer(0)) {
        // if the buffer is not created, create it now.
        VkCommandBufferAllocateInfo cmdBufferCreateInfo = VkCommandBufferAllocateInfo();
        cmdBufferCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufferCreateInfo.pNext = nullptr;
        cmdBufferCreateInfo.commandPool = cmdPool;
        cmdBufferCreateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufferCreateInfo.commandBufferCount = 1;
        CALL_VK(vk::AllocateCommandBuffers(m_device, &cmdBufferCreateInfo, &cmdBuffer));
    }

    // We start by creating and declare the "beginning" our command buffer
    VkCommandBufferBeginInfo cmdBufferBeginInfo = VkCommandBufferBeginInfo();
    cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBufferBeginInfo.pNext = nullptr;
    cmdBufferBeginInfo.flags = 0;
    cmdBufferBeginInfo.pInheritanceInfo = nullptr;
    CALL_VK(vk::BeginCommandBuffer(cmdBuffer, &cmdBufferBeginInfo));

    // transition the buffer into color attachment
    setImageLayout(cmdBuffer, displayImage,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    const VkMpFormatInfo * pFormatInfo = YcbcrVkFormatInfo(inputImageToDrawFrom->imageFormat);
    if (pFormatInfo == NULL) {
        // Non-planar input image.
        setImageLayout(cmdBuffer, inputImageToDrawFrom->image,
                       VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    } else {
        // Multi-planar input image.
        for (uint32_t planeIndx = 0; (planeIndx < (uint32_t)pFormatInfo->planesLayout.numberOfExtraPlanes + 1); planeIndx++) {
            setImageLayout(cmdBuffer, inputImageToDrawFrom->image,
                       VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       (VK_IMAGE_ASPECT_PLANE_0_BIT_KHR << planeIndx));

        }
    }
    // Now we start a renderpass. Any draw command has to be recorded in a renderpass
    VkClearValue clearVals = VkClearValue();
    clearVals.color.float32[0] = 0.0f;
    clearVals.color.float32[1] = 0.34f;
    clearVals.color.float32[2] = 0.90f;
    clearVals.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo renderPassBeginInfo = VkRenderPassBeginInfo();
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.pNext = nullptr;
    renderPassBeginInfo.renderPass = renderPass;
    renderPassBeginInfo.framebuffer = framebuffer;
    renderPassBeginInfo.renderArea = *pRenderArea;
    renderPassBeginInfo.clearValueCount = 1;
    renderPassBeginInfo.pClearValues = &clearVals;

    vk::CmdBeginRenderPass(cmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    // Bind what is necessary to the command buffer
    vk::CmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk::CmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1,
                            pDescriptorSet, 0, nullptr);
    VkDeviceSize offset = 0;
    vk::CmdBindVertexBuffers(cmdBuffer, 0, 1, &pVertexBuffer->get(), &offset);

    bool scaleInput = true;
    TransformPushConstants constants;
    if (scaleInput) {
        if (displayWidth && (displayWidth != inputImageToDrawFrom->imageWidth)) {
            constants.texMatrix[0] = glm::vec2((float)displayWidth / inputImageToDrawFrom->imageWidth, 0.0f);
        }

        if (displayHeight && (displayHeight != inputImageToDrawFrom->imageHeight)) {
            constants.texMatrix[1] = glm::vec2(.0f, (float)displayHeight /inputImageToDrawFrom->imageHeight);
        }
    }

    //upload the matrix to the GPU via push constants
    vk::CmdPushConstants(cmdBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TransformPushConstants), &constants);

    // Draw the quad
    vk::CmdDraw(cmdBuffer, pVertexBuffer->GetNumVertices(), 1, 0, 0);

    vk::CmdEndRenderPass(cmdBuffer);

    setImageLayout(cmdBuffer,
                   displayImage,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);


    if (pFormatInfo == NULL) {
        // Non-planar input image.
        setImageLayout(cmdBuffer, inputImageToDrawFrom->image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    } else {
        // Multi-planar input image.
        for (uint32_t planeIndx = 0; (planeIndx < (uint32_t)pFormatInfo->planesLayout.numberOfExtraPlanes + 1); planeIndx++) {
            setImageLayout(cmdBuffer, inputImageToDrawFrom->image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                       (VK_IMAGE_ASPECT_PLANE_0_BIT_KHR << planeIndx));

        }
    }

    CALL_VK(vk::EndCommandBuffer(cmdBuffer));

    return VK_SUCCESS;
}

VkResult VulkanCommandBuffer::CreateCommandBufferPool(VulkanDeviceInfo* deviceInfo)
{
    DestroyCommandBuffer();
    DestroyCommandBufferPool();

    m_device = deviceInfo->device_;
     // -----------------------------------------------
     // Create a pool of command buffers to allocate command buffer from
     VkCommandPoolCreateInfo cmdPoolCreateInfo = VkCommandPoolCreateInfo();
     cmdPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
     cmdPoolCreateInfo.pNext = nullptr;
     cmdPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
     cmdPoolCreateInfo.queueFamilyIndex = deviceInfo->queueFamilyIndex_;
     CALL_VK(vk::CreateCommandPool(m_device, &cmdPoolCreateInfo, nullptr, &cmdPool));

     return VK_SUCCESS;
}

VkResult VulkanRenderInfo::UpdatePerDrawContexts(VulkanPerDrawContext* pPerDrawContext,
        VkViewport* pViewport, VkRect2D* pScissor, VkRenderPass renderPass,
        const VkSamplerCreateInfo* pSamplerCreateInfo,
        const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{

    LOG(INFO) << "VkVideoUtils: " << "CreateVulkanSamplers " << pPerDrawContext->contextIndex;
    pPerDrawContext->samplerYcbcrConversion.CreateVulkanSampler(m_device, pSamplerCreateInfo,
            pSamplerYcbcrConversionCreateInfo);

    LOG(INFO) << "VkVideoUtils: " << "CreateDescriptorSet " << pPerDrawContext->contextIndex;
    VkSampler immutableSamplers = pPerDrawContext->samplerYcbcrConversion.GetSampler();
    pPerDrawContext->bufferDescriptorSet.CreateDescriptorSet(m_device, 1, &immutableSamplers);

    LOG(INFO) << "VkVideoUtils: " << "CreateGraphicsPipeline " << pPerDrawContext->contextIndex;
    // Create graphics pipeline
    pPerDrawContext->gfxPipeline.CreateGraphicsPipeline(m_device,
            pViewport, pScissor, renderPass,
            &pPerDrawContext->bufferDescriptorSet);

    return VK_SUCCESS;
}


// Create per draw contexts.
VkResult VulkanRenderInfo::CreatePerDrawContexts(VulkanDeviceInfo* deviceInfo,
        VkSwapchainKHR swapchain, const VkExtent2D* pFbExtent2D, VkViewport* pViewport, VkRect2D* pScissor, const VkSurfaceFormatKHR* pSurfaceFormat,
        VkRenderPass renderPass, const VkSamplerCreateInfo* pSamplerCreateInfo,
        const VkSamplerYcbcrConversionCreateInfo* pSamplerYcbcrConversionCreateInfo)
{
    std::vector<VkImage> fbImages;
    vk::get(deviceInfo->getDevice(), swapchain, fbImages);
    int32_t numFbImages = (int32_t )fbImages.size();

    if (mNumCtxs && (mNumCtxs != numFbImages)) {
        if (perDrawCtx) {
            delete [] perDrawCtx;
            perDrawCtx = nullptr;
        }
        mNumCtxs = 0;
    }

    if (mNumCtxs == 0) {
        perDrawCtx = new VulkanPerDrawContext[numFbImages];
        if (!perDrawCtx) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    mNumCtxs = numFbImages;
    m_device = deviceInfo->getDevice();

    for (int32_t ctxsIndx = 0; ctxsIndx < mNumCtxs; ctxsIndx++) {

        VulkanPerDrawContext* pPerDrawContext = GetDrawContext(ctxsIndx);
        pPerDrawContext->contextIndex = ctxsIndx;
        LOG(INFO) << "VkVideoUtils: " << "Init pPerDrawContext " << ctxsIndx;

        LOG(INFO) << "VkVideoUtils: " << "CreateCommandBufferPool " << pPerDrawContext->contextIndex;
        pPerDrawContext->commandBuffer.CreateCommandBufferPool(deviceInfo);

        LOG(INFO) << "VkVideoUtils: " << "CreateFrameBuffer " << pPerDrawContext->contextIndex;
        pPerDrawContext->frameBuffer.CreateFrameBuffer(m_device, swapchain, pFbExtent2D, pSurfaceFormat,
                fbImages[ctxsIndx], renderPass);

        LOG(INFO) << "VkVideoUtils: " << "CreateSyncPrimitives " << pPerDrawContext->contextIndex;
        pPerDrawContext->syncPrimitives.CreateSyncPrimitives(m_device);

        UpdatePerDrawContexts(pPerDrawContext, pViewport, pScissor, renderPass,
                pSamplerCreateInfo, pSamplerYcbcrConversionCreateInfo);
    }

    return VK_SUCCESS;
}

VkResult VulkanRenderInfo::WaitCurrentSwapcahinDraw(VulkanSwapchainInfo* pSwapchainInfo, VulkanPerDrawContext* pPerDrawContext, uint64_t timeoutNsec) {

    return vk::WaitForFences(m_device, 1, &pPerDrawContext->syncPrimitives.mFence, VK_TRUE, timeoutNsec);
}

int32_t VulkanRenderInfo::GetNextSwapchainBuffer(
        VulkanSwapchainInfo* pSwapchainInfo, VulkanPerDrawContext* pPerDrawContext,
        uint64_t timeoutNsec)
{
    lastBuffer = currentBuffer;

    VkSemaphore* pPresentCompleteSemaphore = pSwapchainInfo->GetPresentSemaphoreInFly();

    // Acquire the next image from the swap chain
    VkResult err = vk::AcquireNextImageKHR(m_device, pSwapchainInfo->mSwapchain,
                       UINT64_MAX, *pPresentCompleteSemaphore, VK_NULL_HANDLE,
                       &currentBuffer);

    pSwapchainInfo->SetPresentSemaphoreInFly(currentBuffer, pPresentCompleteSemaphore);

    // Recreate the swapchain if it's no longer compatible with the surface
    // (OUT_OF_DATE) or no longer optimal for presentation (SUBOPTIMAL)
    if ((err == VK_ERROR_OUT_OF_DATE_KHR) || (err == VK_SUBOPTIMAL_KHR)) {
        // pWindowInfo->WindowResize();
        return -1;
    } else {
        CALL_VK(err);
    }

    if (timeoutNsec) {
    // Use a fence to wait until the command buffer has finished execution before
    // using it again
        CALL_VK(vk::WaitForFences(m_device, 1,
                            &pPerDrawContext->syncPrimitives.mFence,
                            VK_TRUE, timeoutNsec));
    }

    return (int32_t)currentBuffer;
}


uint64_t getNsTime(bool resetTime = false)
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
        assert(secs > 0);
        secs--;
        nsec = one_sec_in_ns - (start_.tv_nsec - now.tv_nsec);
    }

    return (secs * one_sec_in_ns) + nsec;
}

// Draw one frame
VkResult VulkanRenderInfo::DrawFrame(VulkanDeviceInfo* deviceInfo,
        VulkanSwapchainInfo* pSwapchainInfo, int64_t presentTimestamp,
        VulkanPerDrawContext* pPerDrawContext,
        uint32_t commandBufferCount)
{
    CALL_VK(vk::ResetFences(deviceInfo->getDevice(), 1, &pPerDrawContext->syncPrimitives.mFence));

    VkPipelineStageFlags waitStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info = VkSubmitInfo();
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = nullptr;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = pSwapchainInfo->GetPresentSemaphore(currentBuffer);
    submit_info.pWaitDstStageMask = &waitStageMask;
    submit_info.commandBufferCount = commandBufferCount;
    submit_info.pCommandBuffers = pPerDrawContext->commandBuffer.getCommandBuffer();
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &pPerDrawContext->syncPrimitives.mRenderCompleteSemaphore;
    CALL_VK(vk::QueueSubmit(deviceInfo->queue_, 1, &submit_info, pPerDrawContext->syncPrimitives.mFence));

    const uint32_t fpsAccumulateFrames = 10;
    const float fpsDividend =
        1000000000LL /* nSec in a Sec */ * fpsAccumulateFrames;
    uint64_t curRealTimeNsecs = getNsTime();
    uint64_t lastRealTimeDeltaNsecs = curRealTimeNsecs - lastRealTimeNsecs;
    lastRealTimeNsecs = curRealTimeNsecs;
    if (0 == (frameId % fpsAccumulateFrames)) {
        uint64_t lastFrameTimeNsecs = frameTimeNsecs;
        frameTimeNsecs = curRealTimeNsecs;

        if (frameId) {
            uint64_t deltaRealTimeNsecs = curRealTimeNsecs - lastFrameTimeNsecs;
            float fps = fpsDividend / deltaRealTimeNsecs;
            printf("####### FPS is %f and average frame time %lld nSec ##############\n", fps,
                 (long long)(deltaRealTimeNsecs / fpsAccumulateFrames));
        }
    }

    if (1) {
        LOG(INFO) << "VkVideoUtils: " << "Drawing frame " <<  frameId << "buffer " << currentBuffer <<
                "curReal: " << (long long)curRealTimeNsecs << "delta: " << (long long)lastRealTimeDeltaNsecs;
    }

    // check VK_KHR_display_control, VK_GOOGLE_display_timing
    VkResult result;
    VkPresentInfoKHR presentInfo = VkPresentInfoKHR();
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &pSwapchainInfo->mSwapchain;
    presentInfo.pImageIndices = &currentBuffer;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &pPerDrawContext->syncPrimitives.mRenderCompleteSemaphore;
    presentInfo.pResults = &result;

#ifdef VK_GOOGLE_display_timing
    VkPresentTimeGOOGLE presentTime;
    VkPresentTimesInfoGOOGLE presentTimesInfo;
    if (pSwapchainInfo->mDisplayTiming.DisplayTimingIsEnabled()) {
        presentTime.presentID = frameId;
        presentTime.desiredPresentTime = presentTimestamp;

        presentTimesInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        presentTimesInfo.pNext = nullptr;
        presentTimesInfo.swapchainCount = 1;
        presentTimesInfo.pTimes = &presentTime;

        presentInfo.pNext = &presentTimesInfo;
    }
#endif // VK_GOOGLE_display_timing
    vk::QueuePresentKHR(deviceInfo->queue_, &presentInfo);

    frameId++;

    return VK_SUCCESS;
}

VkResult AllocateMemoryTypeFromProperties(VulkanDeviceInfo* deviceInfo,
        uint32_t typeBits,
        VkFlags requirements_mask,
        uint32_t *typeIndex)
{
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((deviceInfo->gpuMemoryProperties_.memoryTypes[i].propertyFlags &
                    requirements_mask) == requirements_mask) {
                *typeIndex = i;
                return VK_SUCCESS;
            }
        }
        typeBits >>= 1;
    }
    // No memory types matched, return failure
    return VK_ERROR_MEMORY_MAP_FAILED;
}

bool MapMemoryTypeToIndex(VkPhysicalDevice gpuDevice, uint32_t typeBits,
                          VkFlags requirements_mask, uint32_t *typeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vk::GetPhysicalDeviceMemoryProperties(gpuDevice, &memoryProperties);
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((memoryProperties.memoryTypes[i].propertyFlags & requirements_mask) ==
                    requirements_mask) {
                *typeIndex = i;
                return true;
            }
        }
        typeBits >>= 1;
    }
    return false;
}

void setImageLayout(VkCommandBuffer cmdBuffer, VkImage image,
                    VkImageLayout oldImageLayout, VkImageLayout newImageLayout,
                    VkPipelineStageFlags srcStages,
                    VkPipelineStageFlags destStages, VkImageAspectFlags aspectMask)
{
    VkImageMemoryBarrier2KHR imageMemoryBarrier = VkImageMemoryBarrier2KHR();
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
    imageMemoryBarrier.pNext = nullptr;
    imageMemoryBarrier.srcStageMask  = srcStages;
    imageMemoryBarrier.srcAccessMask = 0;
    imageMemoryBarrier.dstStageMask  = destStages;
    imageMemoryBarrier.dstAccessMask = 0;
    imageMemoryBarrier.oldLayout = oldImageLayout;
    imageMemoryBarrier.newLayout = newImageLayout;
    imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange.aspectMask = aspectMask;
    imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
    imageMemoryBarrier.subresourceRange.levelCount = 1;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
    imageMemoryBarrier.subresourceRange.layerCount = 1;

    switch ((uint32_t)oldImageLayout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        break;
    default:
        break;
    }

    switch ((uint32_t)newImageLayout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        imageMemoryBarrier.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        break;

    case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR;
        break;

    case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        break;

    case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
        break;

    case VK_IMAGE_LAYOUT_GENERAL:
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        break;

    default:
        break;
    }

    const VkDependencyInfoKHR dependencyInfo = {
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
            nullptr,
            VK_DEPENDENCY_BY_REGION_BIT,
            0, nullptr,
            0, nullptr,
            1, &imageMemoryBarrier,
    };

    vk::CmdPipelineBarrier2KHR(cmdBuffer, &dependencyInfo);
}


VkResult VkVideoAppCtx::CreateSamplerYcbcrConversions()
{

    return VK_SUCCESS;
}

NativeHandle::NativeHandle (void) :
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
      m_fd(-1),
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_androidHardwareBuffer(NULL),
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_externalMemoryHandleType(VkExternalMemoryHandleTypeFlagBits(0))
{
}

NativeHandle::NativeHandle (const NativeHandle& other) :
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
      m_fd(-1),
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_androidHardwareBuffer(NULL),
#endif
    m_externalMemoryHandleType(VkExternalMemoryHandleTypeFlagBits(0))
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) && (other.m_fd >= 0)) {
        assert(m_fd >= 0);
    }
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    else if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) && other.m_androidHardwareBuffer) {
        m_androidHardwareBuffer = other.m_androidHardwareBuffer;
        assert(m_androidHardwareBuffer);
    }
#endif //defined(VK_ANDROID_external_memory_android_hardware_buffer)
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
}

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
NativeHandle::NativeHandle (int fd) :
    m_fd                      (fd),
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_androidHardwareBuffer   (NULL),
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_externalMemoryHandleType(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
{
}
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)

#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
NativeHandle::NativeHandle (AHardwareBufferHandle buffer) :
    m_fd                      (-1),
    m_androidHardwareBuffer   (buffer),
    m_externalMemoryHandleType(VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)
{
}
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)

NativeHandle::~NativeHandle (void)
{
    // Release the object reference.
    releaseReference();
}

void NativeHandle::releaseReference (void)
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) && (m_fd >= 0)) {
        ::close(m_fd);
    }
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) && m_androidHardwareBuffer) {
        NvReleaseHardwareBufferHandle(m_androidHardwareBuffer);
    }
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)

    disown();
}

bool NativeHandle::isValid (void) const
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) && (m_fd >= 0)) {
        return true;
    }
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    if ((m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) && m_androidHardwareBuffer) {
        return true;
    }
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)

    return false;
}

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
NativeHandle& NativeHandle::operator= (int fd)
{
    releaseReference();
    m_fd = fd;
    m_externalMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    return *this;
}
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)

#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
NativeHandle& NativeHandle::operator= (AHardwareBufferHandle buffer)
{
    releaseReference();
    m_androidHardwareBuffer = buffer;
    m_externalMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    return *this;
}
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)

void NativeHandle::disown (void)
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
    m_fd = -1;
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_androidHardwareBuffer = AHardwareBufferHandle(NULL);
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)
    m_externalMemoryHandleType = VkExternalMemoryHandleTypeFlagBits(0);
}

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)
int NativeHandle::getFd (void) const
{
    assert(m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    return m_fd;
}
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_PLATFORM_IS_UNIX)

#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
AHardwareBufferHandle NativeHandle::getAndroidHardwareBuffer (void) const
{
    assert(m_fd == -1);
    assert(m_externalMemoryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    return m_androidHardwareBuffer;
}
#endif // defined(VK_ANDROID_external_memory_android_hardware_buffer)

NativeHandle NativeHandle::InvalidNativeHandle = NativeHandle();

} // namespace vulkanVideoUtils
