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


#define LOG_TAG "VulkanVideoRender"
#include "pattern.h"
#include <android/log.h>
#include "vulkan_wrapper.h"

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>

#include "VulkanVideoRender.h"
#include "VulkanVideoUtils.h"

#include "vulkan/vk_ahb_utils.h"
#if defined(ANDROID_EXTERNAL_HWBUFFER_SUPPORT)
#include <hardware/gralloc1.h>
#include <vndk/hardware_buffer.h>
#include <private/android/AHardwareBufferHelpers.h>
#include <android/hardware/graphics/common/1.0/types.h>
#else
#include "vulkan/vk_ahb_compat.h"
#endif

// Android log function wrappers
static const char *kTAG = "VulkanVideoRender";
#define LOGI(...) \
  ((void)__android_log_print(ANDROID_LOG_INFO, kTAG, __VA_ARGS__))
#define LOGW(...) \
  ((void)__android_log_print(ANDROID_LOG_WARN, kTAG, __VA_ARGS__))
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, kTAG, __VA_ARGS__))

// Vulkan call wrapper
#define CALL_VK(func)                                                 \
  if (VK_SUCCESS != (func)) {                                         \
    __android_log_print(ANDROID_LOG_ERROR, "Tutorial ",               \
                        "Vulkan error. File[%s], line[%d]", __FILE__, \
                        __LINE__);                                    \
    assert(false);                                                    \
  }

// A macro to check value is VK_SUCCESS
// Used also for non-vulkan functions but return VK_SUCCESS
#define VK_CHECK(x) CALL_VK(x)

using namespace android;
namespace vulkanVideoUtils {

static const VkSamplerCreateInfo defaultSamplerInfo = {
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR,  VK_FILTER_LINEAR,  VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor                   unnormalizedCoordinates
                 0.0,          false,            0.00,         false,       VK_COMPARE_OP_NEVER,   0.0,   16.0,    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,        false };

// InitVulkan:
//   Initialize Vulkan Context when android application window is created
//   upon return, vulkan is ready to draw frames
VkResult VulkanVideoRender::Init(VkFormat imageFormat, int32_t videoWidth, int32_t videoHeight, uint32_t format, android_dataspace dataSpace)
{
    // Create the Video app context where all the state is contained.
    LOGI("-> Create Video App context");
    pVkVideoAppCtx = new VkVideoAppCtx(useTestImages);
    if (!pVkVideoAppCtx) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Initialize/import the Vulkan's APIs.
    LOGI("-> InitVulkanWrapper");
    if (!InitVulkanWrapper()) {
        LOGW("Vulkan is unavailable, install vulkan and re-start");
        return VK_NOT_READY;
    }

    // Now, create the vulkan's instance and a device/queues that are used for rendering.
    LOGI("-> CreateVulkanDevice");
    VkApplicationInfo appInfo = VkApplicationInfo();
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext = nullptr;
    appInfo.apiVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pApplicationName = "vulkan_video_demo";
    appInfo.pEngineName = "vulkan_demo";
    // create a device
    pVkVideoAppCtx->device_.CreateVulkanDevice(&appInfo);

    // Create Vulkan's Vertex buffer
    LOGI("-> Create Vertex Buffer");
    // position/texture coordinate pair per vertex.
    static const Vertex vertices[4] = {
        { {  1.0f,  1.0f }, { 1.0f, 1.0f }, },
        { { -1.0f,  1.0f }, { 0.0f, 1.0f }, },
        { { -1.0f, -1.0f }, { 0.0f, 0.0f }, },
        { {  1.0f, -1.0f }, { 1.0f, 0.0f }, }

    };
    CALL_VK(pVkVideoAppCtx->vertexBuffer_.CreateVertexBuffer(&pVkVideoAppCtx->device_,
            (float*)vertices, sizeof(vertices),
            sizeof(vertices)/sizeof(vertices[0])));

    // Create a native window from the OS.
    LOGI("-> CreateWindowSurface");
    if((status_t)OK != pVkVideoAppCtx->window.CreateWindowSurface(videoWidth, videoHeight)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    // Now, create a Vulkan's swapchain against the native window.
    LOGI("-> CreateSwapChain");
    pVkVideoAppCtx->swapchain_.CreateSwapChain(&pVkVideoAppCtx->device_,
            pVkVideoAppCtx->window.GetAndroidNativeWindow());

    // Create test image, if this is enabled.
    LOGI("-> Create Test Image");
    if (pVkVideoAppCtx->useTestImage_) {
        VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.pNext = NULL;
        imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        imageCreateInfo.format = imageFormat;
        imageCreateInfo.extent = {
                pVkVideoAppCtx->swapchain_.mDisplaySize.width,
                pVkVideoAppCtx->swapchain_.mDisplaySize.height,
                1};
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.queueFamilyIndexCount = 1;
        imageCreateInfo.pQueueFamilyIndices = &pVkVideoAppCtx->device_.queueFamilyIndex_;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        imageCreateInfo.flags = 0;
        pVkVideoAppCtx->testFrameImage_.CreateImage(&pVkVideoAppCtx->device_, &imageCreateInfo,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                ColorPatternColorBars,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    }

    // Create per a frame draw context num == mSwapchainNumBufs.
    LOGI("-> CreatePerDrawContexts");
    const VkSamplerYcbcrConversionCreateInfo &defaultSamplerYcbcrConversionCreateInfo = {
                   VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL, imageFormat,
                   VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
                   { VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY },
                     VK_CHROMA_LOCATION_MIDPOINT, VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, false };

    // Create Vulkan's RenderPass
    LOGI("-> CreateRenderPass");
    pVkVideoAppCtx->renderPass_.CreateRenderPass(&pVkVideoAppCtx->device_, &pVkVideoAppCtx->swapchain_);

    pVkVideoAppCtx->render_.CreatePerDrawContexts(&pVkVideoAppCtx->device_,
            &pVkVideoAppCtx->swapchain_, pVkVideoAppCtx->renderPass_.getRenderPass(),
            pVkVideoAppCtx->swapchain_.mSwapchainNumBufs, &defaultSamplerInfo, &defaultSamplerYcbcrConversionCreateInfo);

    pVkVideoAppCtx->ContextIsReady();

    LOGI("<- InitVulkan Done");
    return VK_SUCCESS;
}

void VulkanVideoRender::Destroy()
{
    if (!pVkVideoAppCtx || !pVkVideoAppCtx->IsContextReady()) {
        return;
    }

    LOGI("vkDeviceWaitIdle before destroy");
    pVkVideoAppCtx->device_.DeviceWaitIdle();

    delete pVkVideoAppCtx;
    pVkVideoAppCtx = NULL;
}

static const bool debugFrameData = false;
static const bool debugFrameDataVerbose = false;

// Draw one frame
VkResult VulkanVideoRender::DrawFrame(sp<PinnedBufferItem>& inPinnedBufferItem,
                                      sp<PinnedBufferItem>& outPinnedBufferItem)
{
    if (!pVkVideoAppCtx || !pVkVideoAppCtx->IsContextReady()) {
        return VK_NOT_READY;
    }

    if (!inPinnedBufferItem.get() || inPinnedBufferItem->getBufferItem().mSlot < 0) {
         return VK_NOT_READY;
    }

    pVkVideoAppCtx->render_.GotFrame();

    bool skipLateFrames = false;
    uint64_t refreshDuration = 0;
    pVkVideoAppCtx->swapchain_.GetDisplayRefreshCycle(&refreshDuration);
    LOGI("refreshDuration is %llu nSec", (unsigned long long)refreshDuration);

    int64_t presentTimestamp = inPinnedBufferItem->getBufferItem().mTimestamp;
    int64_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    int64_t deltaTime = presentTimestamp - currentTime;
    if (debugFrameData || (deltaTime <  0)) {
        printf("Current %lld, present %lld delta %lld times ns\n", (long long)currentTime,
            (long long)presentTimestamp,
            (long long)(deltaTime));
    }
    if (skipLateFrames && (deltaTime <  0)) {
        outPinnedBufferItem = inPinnedBufferItem;
        uint32_t skipped = pVkVideoAppCtx->render_.SkippedFrame();
        // This frame has arrived too late - skip its presentation and return it back to the codec.
        printf("\tThis frame %llu has arrived too late %lld: deltaTime. Please check the display FPS rate!!! Skipping this frame ... Skipped %u of total %llu\n",
                (unsigned long long)inPinnedBufferItem->getBufferItem().mFrameNumber, (long long)(deltaTime),
                skipped, (unsigned long long)pVkVideoAppCtx->render_.GetTotalFrames());

        return VK_SUCCESS;
    }

    bool useTestImage = pVkVideoAppCtx->useTestImage_;

    const uint32_t numImages = sizeof(pVkVideoAppCtx->frameImages_)/sizeof(pVkVideoAppCtx->frameImages_[0]);
    const int32_t inputBufferIndex = inPinnedBufferItem->getBufferItem().mSlot;
    assert(inputBufferIndex < numImages);
    ImageObject* pNewInputImage = &pVkVideoAppCtx->frameImages_[inputBufferIndex];

    if (debugFrameData ) {
        LOGI("DrawFrame inputBufferIndex %d",inputBufferIndex);
    }
    const int32_t currentScBuffer = pVkVideoAppCtx->render_.GetNextSwapchainBuffer(
            &pVkVideoAppCtx->window, &pVkVideoAppCtx->swapchain_, nullptr, 0 /* do not wait */);
    if (currentScBuffer < 0) {
        return VK_NOT_READY;
    }
    bool waitedOnScFrame = false;

    VulkanPerDrawContext* pPerDrawContext = pVkVideoAppCtx->render_.GetDrawContext(currentScBuffer);
    if (pPerDrawContext == NULL) {
        return VK_NOT_READY;
    }

    AHardwareBufferHandle  newAndroidHardwareBuffer = AHardwareBuffer_from_GraphicBuffer(inPinnedBufferItem->getBufferItem().mGraphicBuffer.get());
    VkAndroidHardwareBufferFormatPropertiesANDROID bufferFormatProperties = VkAndroidHardwareBufferFormatPropertiesANDROID();
    bufferFormatProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    bufferFormatProperties.pNext = nullptr;

    VkAndroidHardwareBufferPropertiesANDROID bufferProperties = VkAndroidHardwareBufferPropertiesANDROID();
    bufferProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProperties.pNext = &bufferFormatProperties;
    VkResult result = vkGetAndroidHardwareBufferPropertiesANDROID(pVkVideoAppCtx->device_.getDevice(), newAndroidHardwareBuffer, &bufferProperties);
    if (result == VK_SUCCESS) {
        if (debugFrameDataVerbose) {
            ALOGD(  "\tInput Buffer bufferFormatProperties:\n"
                "\t\tallocationSize %llu, memoryTypeBits 0x%x"
                "\t\tvkFormat 0x%x, extFormat 0x%llx, formatFeatures 0x%x"
                "\t\tycbcrModel 0x%x,  ycbcrRange 0x%x\n"
                "\t\tcomponents.r 0x%x,components.g 0x%x components.b 0x%x components.a 0x%x\n"
                "\t\txChromaOffset %u, yChromaOffset %u",
                (unsigned long long)bufferProperties.allocationSize, bufferProperties.memoryTypeBits,
                bufferFormatProperties.format, (unsigned long long)bufferFormatProperties.externalFormat, bufferFormatProperties.formatFeatures,
                bufferFormatProperties.suggestedYcbcrModel, bufferFormatProperties.suggestedYcbcrRange,
                bufferFormatProperties.samplerYcbcrConversionComponents.r, bufferFormatProperties.samplerYcbcrConversionComponents.g,
                bufferFormatProperties.samplerYcbcrConversionComponents.b, bufferFormatProperties.samplerYcbcrConversionComponents.a,
                bufferFormatProperties.suggestedXChromaOffset, bufferFormatProperties.suggestedYChromaOffset );
        }
    }

    const VkSamplerYcbcrConversionCreateInfo newSamplerYcbcrConversionCreateInfo = {
                   VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL, bufferFormatProperties.format,
                   bufferFormatProperties.suggestedYcbcrModel, bufferFormatProperties.suggestedYcbcrRange,
                   { bufferFormatProperties.samplerYcbcrConversionComponents },
                   bufferFormatProperties.suggestedXChromaOffset, bufferFormatProperties.suggestedYChromaOffset, VK_FILTER_NEAREST, false };

    bool samplerRequiresUpdate = pPerDrawContext->samplerYcbcrConversion.SamplerRequiresUpdate(nullptr, &newSamplerYcbcrConversionCreateInfo);
    if (samplerRequiresUpdate) {
        ALOGD("\tSampler %d needs an update!", pPerDrawContext->contextIndex);
        pVkVideoAppCtx->render_.UpdatePerDrawContexts(pPerDrawContext,
                    &pVkVideoAppCtx->swapchain_, pVkVideoAppCtx->renderPass_.getRenderPass(), &defaultSamplerInfo, &newSamplerYcbcrConversionCreateInfo);
    } else {
        if (debugFrameDataVerbose) {
            ALOGD("\tSampler %d does NOT require an update.", pPerDrawContext->contextIndex);
        }
    }

    // If the image we are about to replace is in use by the SC, wait for the current buffer to finish.
    if (pPerDrawContext->pCurrentImage && pPerDrawContext->pCurrentImage->inUseBySwapchain) {
        CALL_VK(pVkVideoAppCtx->render_.WaitCurrentSwapcahinDraw(&pVkVideoAppCtx->swapchain_, pPerDrawContext));
        pPerDrawContext->pCurrentImage->inUseBySwapchain = false;
        waitedOnScFrame = true;
    }

    bool imageRequiresUpdate = true;
    if ((pNewInputImage->imageFormat != VK_FORMAT_UNDEFINED) &&
        (pNewInputImage->imageFormat == bufferFormatProperties.format) &&
        (pNewInputImage->bufferHandle != nullptr) &&
        (pNewInputImage->bufferHandle == inPinnedBufferItem->getBufferItem().mGraphicBuffer->handle)) {

        imageRequiresUpdate = false;
    }

    if (imageRequiresUpdate) {
        if (debugFrameData ) {
            LOGI("-> Create and Import a new image for inputBufferIndex %d", inputBufferIndex);
        }
        VkImageCreateInfo imageCreateInfo;
        AndroidGetVkFormatAndYcbcrInfo(inPinnedBufferItem->getBufferItem().mGraphicBuffer->handle,
                NULL, &imageCreateInfo, NULL);

        assert(imageCreateInfo.extent.width  == inPinnedBufferItem->getBufferItem().mGraphicBuffer->getWidth());
        assert(imageCreateInfo.extent.height ==  inPinnedBufferItem->getBufferItem().mGraphicBuffer->getHeight());
        pNewInputImage->CreateImage(&pVkVideoAppCtx->device_, &imageCreateInfo,
                0,
                ColorPatternClear,
                VkExternalMemoryHandleTypeFlags(),
                newAndroidHardwareBuffer);
    }

    // Wait here, instead of doing it ahead of time to amortize the processing cost.
    // The wait must happen before the descriptor write and command buffer update.

    if (!waitedOnScFrame) {
        CALL_VK(pVkVideoAppCtx->render_.WaitCurrentSwapcahinDraw(&pVkVideoAppCtx->swapchain_, pPerDrawContext));
    }
    if (pPerDrawContext->pCurrentImage) {
        pPerDrawContext->pCurrentImage->inUseBySwapchain = false;
    }
    pPerDrawContext->pCurrentImage = pNewInputImage;
    outPinnedBufferItem = pPerDrawContext->currentInputBuffer;
    pPerDrawContext->currentInputBuffer = inPinnedBufferItem;

    if (debugFrameData ) {
        LOGI("-> WriteDescriptorSetAtIndex FB %d inputBufferIndex %d", currentScBuffer, inputBufferIndex);
    }
    pPerDrawContext->bufferDescriptorSet.WriteDescriptorSet(VkSampler(0), pNewInputImage->view);

    VkRect2D renderArea = {{0, 0}, pVkVideoAppCtx->swapchain_.mDisplaySize};
    pPerDrawContext->commandBuffer.CreateCommandBuffer(
                    pVkVideoAppCtx->renderPass_.getRenderPass(), pNewInputImage,
                    pVkVideoAppCtx->swapchain_.mDisplayImages[currentScBuffer], pPerDrawContext->frameBuffer.GetFrameBuffer(), &renderArea,
                    pPerDrawContext->gfxPipeline.getPipeline(),
                    pPerDrawContext->bufferDescriptorSet.getPipelineLayout(), pPerDrawContext->bufferDescriptorSet.getDescriptorSet(),
                    &pVkVideoAppCtx->vertexBuffer_);

    if (debugFrameData ) {
        LOGI("Drawing FB %d inputBufferIndex %d", currentScBuffer, inputBufferIndex);
    }

    return pVkVideoAppCtx->render_.DrawFrame(&pVkVideoAppCtx->device_, &pVkVideoAppCtx->swapchain_, presentTimestamp, pPerDrawContext);
}


// Draw one frame
VkResult VulkanVideoRender::DrawTestFrame(int32_t inputBufferIndex)
{
    if (!pVkVideoAppCtx || !pVkVideoAppCtx->IsContextReady()) {
        return VK_NOT_READY;
    }

    const uint32_t numImages = sizeof(pVkVideoAppCtx->frameImages_)/sizeof(pVkVideoAppCtx->frameImages_[0]);
    inputBufferIndex = (inputBufferIndex < 0) ? (pVkVideoAppCtx->render_.getframeId() % numImages) : inputBufferIndex;

    assert(inputBufferIndex < numImages);
    ImageObject* pNewInputImage = &pVkVideoAppCtx->frameImages_[inputBufferIndex];

    LOGI("DrawTestFrame inputBufferIndex %d",inputBufferIndex);
    const int32_t currentScBuffer = pVkVideoAppCtx->render_.GetNextSwapchainBuffer(
            &pVkVideoAppCtx->window, &pVkVideoAppCtx->swapchain_, nullptr, 0 /* do not wait */);
    if (currentScBuffer < 0) {
        return VK_NOT_READY;
    }
    bool waitedOnScFrame = false;

    VulkanPerDrawContext* pPerDrawContext = pVkVideoAppCtx->render_.GetDrawContext(currentScBuffer);
    if (pPerDrawContext == NULL) {
        return VK_NOT_READY;
    }

    AHardwareBufferHandle  newAndroidHardwareBuffer = pVkVideoAppCtx->testFrameImage_.ExportHandle();
    VkAndroidHardwareBufferFormatPropertiesANDROID bufferFormatProperties = VkAndroidHardwareBufferFormatPropertiesANDROID();
    bufferFormatProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    bufferFormatProperties.pNext = nullptr;

    VkAndroidHardwareBufferPropertiesANDROID bufferProperties = VkAndroidHardwareBufferPropertiesANDROID();
    bufferProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProperties.pNext = &bufferFormatProperties;
    VkResult result = vkGetAndroidHardwareBufferPropertiesANDROID(pVkVideoAppCtx->device_.getDevice(), newAndroidHardwareBuffer, &bufferProperties);
    if (result == VK_SUCCESS) {
        ALOGD(  "\tInput Buffer bufferFormatProperties:\n"
                "\t\tallocationSize %llu, memoryTypeBits 0x%x"
                "\t\tvkFormat 0x%x, extFormat 0x%llx, formatFeatures 0x%x"
                "\t\tycbcrModel 0x%x,  ycbcrRange 0x%x\n"
                "\t\tcomponents.r 0x%x,components.g 0x%x components.b 0x%x components.a 0x%x\n"
                "\t\txChromaOffset %u, yChromaOffset %u",
                (unsigned long long)bufferProperties.allocationSize, bufferProperties.memoryTypeBits,
                bufferFormatProperties.format, (unsigned long long)bufferFormatProperties.externalFormat, bufferFormatProperties.formatFeatures,
                bufferFormatProperties.suggestedYcbcrModel, bufferFormatProperties.suggestedYcbcrRange,
                bufferFormatProperties.samplerYcbcrConversionComponents.r, bufferFormatProperties.samplerYcbcrConversionComponents.g,
                bufferFormatProperties.samplerYcbcrConversionComponents.b, bufferFormatProperties.samplerYcbcrConversionComponents.a,
                bufferFormatProperties.suggestedXChromaOffset, bufferFormatProperties.suggestedYChromaOffset );
    }

    const VkSamplerYcbcrConversionCreateInfo newSamplerYcbcrConversionCreateInfo = {
                   VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL, bufferFormatProperties.format,
                   (bufferFormatProperties.suggestedYcbcrModel == VK_SAMPLER_YCBCR_MODEL_CONVERSION_MAX_ENUM) ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 : bufferFormatProperties.suggestedYcbcrModel,
                   (bufferFormatProperties.suggestedYcbcrRange == VK_SAMPLER_YCBCR_RANGE_MAX_ENUM) ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : bufferFormatProperties.suggestedYcbcrRange,
                   { bufferFormatProperties.samplerYcbcrConversionComponents },
                   bufferFormatProperties.suggestedXChromaOffset, bufferFormatProperties.suggestedYChromaOffset, VK_FILTER_NEAREST, false };

    bool samplerRequiresUpdate = pPerDrawContext->samplerYcbcrConversion.SamplerRequiresUpdate(nullptr, &newSamplerYcbcrConversionCreateInfo);
    if (samplerRequiresUpdate) {
        ALOGD("\tSampler %d needs an update!", pPerDrawContext->contextIndex);
        pVkVideoAppCtx->render_.UpdatePerDrawContexts(pPerDrawContext,
                    &pVkVideoAppCtx->swapchain_, pVkVideoAppCtx->renderPass_.getRenderPass(), &defaultSamplerInfo, &newSamplerYcbcrConversionCreateInfo);
    } else {
        ALOGD("\tSampler %d does NOT require an update.", pPerDrawContext->contextIndex);
    }

    // If the image we are about to replace is in use by the SC, wait for the current buffer to finish.
    if (pPerDrawContext->pCurrentImage && pPerDrawContext->pCurrentImage->inUseBySwapchain) {
        CALL_VK(pVkVideoAppCtx->render_.WaitCurrentSwapcahinDraw(&pVkVideoAppCtx->swapchain_, pPerDrawContext));
        pPerDrawContext->pCurrentImage->inUseBySwapchain = false;
        waitedOnScFrame = true;
    }

    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(newAndroidHardwareBuffer);
    bool imageRequiresUpdate = true;
    if ((pNewInputImage->imageFormat != VK_FORMAT_UNDEFINED) &&
        (pNewInputImage->imageFormat == bufferFormatProperties.format) &&
        (pNewInputImage->bufferHandle != nullptr) &&
        (pNewInputImage->bufferHandle == handle)) {

        imageRequiresUpdate = false;
    }

    if (imageRequiresUpdate) {

        LOGI("-> Create and Import a new image for inputBufferIndex %d", inputBufferIndex);
        VkImageCreateInfo imageCreateInfo;
        // This image does not exist yet - import it.
        AndroidGetVkFormatAndYcbcrInfo(handle, NULL, &imageCreateInfo, NULL);
        pNewInputImage->CreateImage(&pVkVideoAppCtx->device_, &imageCreateInfo,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                ColorPatternClear,
                VkExternalMemoryHandleTypeFlags(),
                newAndroidHardwareBuffer);
    } else {
        AHardwareBuffer_release(newAndroidHardwareBuffer);
    }

    // Wait here, instead of doing it ahead of time to amortize the processing cost.
    // The wait must happen before the descriptor write and command buffer update.

    if (!waitedOnScFrame) {
        CALL_VK(pVkVideoAppCtx->render_.WaitCurrentSwapcahinDraw(&pVkVideoAppCtx->swapchain_, pPerDrawContext));
    }
    if (pPerDrawContext->pCurrentImage) {
        pPerDrawContext->pCurrentImage->inUseBySwapchain = false;
    }
    pPerDrawContext->pCurrentImage = pNewInputImage;

    LOGI("-> WriteDescriptorSetAtIndex FB %d inputBufferIndex %d", currentScBuffer, inputBufferIndex);
    pPerDrawContext->bufferDescriptorSet.WriteDescriptorSet(VkSampler(0), pNewInputImage->view);

    VkRect2D renderArea = {{0, 0}, pVkVideoAppCtx->swapchain_.mDisplaySize};
    pPerDrawContext->commandBuffer.CreateCommandBuffer(
                    pVkVideoAppCtx->renderPass_.getRenderPass(), pNewInputImage,
                    pVkVideoAppCtx->swapchain_.mDisplayImages[currentScBuffer], pPerDrawContext->frameBuffer.GetFrameBuffer(), &renderArea,
                    pPerDrawContext->gfxPipeline.getPipeline(),
                    pPerDrawContext->bufferDescriptorSet.getPipelineLayout(), pPerDrawContext->bufferDescriptorSet.getDescriptorSet(),
                    &pVkVideoAppCtx->vertexBuffer_);

    LOGI("Drawing FB %d inputBufferIndex %d", currentScBuffer, inputBufferIndex);
    return pVkVideoAppCtx->render_.DrawFrame(&pVkVideoAppCtx->device_, &pVkVideoAppCtx->swapchain_, 0, pPerDrawContext);
}

} // vulkanVideoUtils
