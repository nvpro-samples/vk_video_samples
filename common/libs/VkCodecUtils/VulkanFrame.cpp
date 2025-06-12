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

#include <array>
#include <string>
#include <vector>
#include <iostream>
#include <thread>  // Added for std::this_thread::sleep_for

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkShell/Shell.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VulkanFrame.h"
#include "VkVideoCore/DecodeFrameBufferIf.h"
#include "VkCodecUtils/VulkanSemaphoreDump.h"

template<class FrameDataType>
VulkanFrame<FrameDataType>::VulkanFrame(const VulkanDeviceContext* vkDevCtx)
    : FrameProcessor(false)
    , m_refCount(0)
    , m_vkDevCtx(vkDevCtx)
    , m_videoQueue()
    , m_samplerYcbcrModelConversion(VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709)
    , m_samplerYcbcrRange(VK_SAMPLER_YCBCR_RANGE_ITU_NARROW)
    , m_videoRenderer(nullptr)
    , m_codecPaused(false)
    , m_gfxQueue()
    , m_vkFormat()
    , m_physicalDevProps()
    , m_frameData()
    , m_frameDataIndex()
{
}

template<class FrameDataType>
VulkanFrame<FrameDataType>::~VulkanFrame()
{
    DetachShell();
}

template<class FrameDataType>
int VulkanFrame<FrameDataType>::AttachQueue(VkSharedBaseObj<VkVideoRefCountBase>& videoQueue)
{
    m_videoQueue = (VkSharedBaseObj<VkVideoQueue<FrameDataType>>&)videoQueue;

    return 0;
}

template<class FrameDataType>
int VulkanFrame<FrameDataType>::AttachShell(const Shell& sh)
{
    const Shell::Context& ctx = sh.GetContext();
    m_gfxQueue = ctx.devCtx->GetGfxQueue();

    m_vkDevCtx->GetPhysicalDeviceProperties(ctx.devCtx->getPhysicalDevice(), &m_physicalDevProps);

    const uint32_t apiMajorVersion = VK_API_VERSION_MAJOR(m_physicalDevProps.apiVersion);
    const uint32_t apiMinorVersion = VK_API_VERSION_MINOR(m_physicalDevProps.apiVersion);
    const uint32_t apiPatchVersion = VK_API_VERSION_PATCH(m_physicalDevProps.apiVersion);

    if (m_physicalDevProps.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 199)) {
        std::cerr << std::endl << "Incompatible Vulkan API version: " << apiMajorVersion << "." << apiMinorVersion << "." << apiPatchVersion << std::endl;
        std::cerr << "Info: Driver version is: " << m_physicalDevProps.driverVersion << std::endl;
        std::cerr << "Please upgrade your driver. The version supported is: 1.2.199 or later aka " << std::hex << VK_MAKE_API_VERSION(0, 1, 2, 199) << std::endl;
        assert(!"Incompatible API version - please upgrade your driver.");
        return -1;
    }

    const bool useTestImage = ((m_vkDevCtx->GetVideoDecodeQueue() == VkQueue()) &&
                               (m_vkDevCtx->GetVideoEncodeQueue() == VkQueue()));
    m_videoRenderer = new vulkanVideoUtils::VkVideoAppCtx(useTestImage);
    if (m_videoRenderer == nullptr) {
        return -1;
    }

    m_videoRenderer->m_vkDevCtx = m_vkDevCtx;

    m_vkFormat = ctx.format.format;

    CreateFrameData((int32_t)ctx.backBuffers.size());

    // Create Vulkan's Vertex buffer
    // position/texture coordinate pair per vertex.
    static const vk::Vertex vertices[4] = {
       //    Vertex         Texture coordinate
        { {  1.0f,  1.0f }, { 1.0f, 1.0f }, },
        { { -1.0f,  1.0f }, { 0.0f, 1.0f }, },
        { { -1.0f, -1.0f }, { 0.0f, 0.0f }, },
        { {  1.0f, -1.0f }, { 1.0f, 0.0f }, },

    };

    if (VK_SUCCESS != m_videoRenderer->m_vertexBuffer.CreateVertexBuffer(m_videoRenderer->m_vkDevCtx,
                                                               (const float*)vertices, sizeof(vertices),
                                                               sizeof(vertices) / sizeof(vertices[0]))) {

        std::cerr << "VulkanVideoFrame: " << "File " << __FILE__ << "line " << __LINE__;
        return -1;
    }

    return 0;
}

template<class FrameDataType>
void VulkanFrame<FrameDataType>::DetachShell()
{
    DestroyFrameData();

    delete m_videoRenderer;
    m_videoRenderer = nullptr;
}

template<class FrameDataType>
int32_t VulkanFrame<FrameDataType>::CreateFrameData(int32_t count)
{
    m_frameData.resize(count);

    m_frameDataIndex = 0;

    for (auto& data : m_frameData) {
        data.Reset();
    }

    if (m_frameData.size() >= (size_t)count) {
        return count;
    }

    return -1;
}

template<class FrameDataType>
void VulkanFrame<FrameDataType>::DestroyFrameData()
{
    for (auto& data : m_frameData) {
        data.Reset();
    }

    m_frameData.clear();
}

static const VkSamplerCreateInfo defaultSamplerInfo = {
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor
    // unnormalizedCoordinates
    0.0, false, 0.00, false, VK_COMPARE_OP_NEVER, 0.0, 16.0, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, false
};

template<class FrameDataType>
int VulkanFrame<FrameDataType>::AttachSwapchain(const Shell& sh)
{
    const Shell::Context& ctx = sh.GetContext();

    PrepareViewport(ctx.extent);

    uint32_t imageWidth  = (m_videoQueue->GetWidth() > 0)  ? m_videoQueue->GetWidth()  : m_scissor.extent.width;
    uint32_t imageHeight = (m_videoQueue->GetHeight() > 0) ? m_videoQueue->GetHeight() : m_scissor.extent.height;
    VkFormat imageFormat = m_videoQueue->GetFrameImageFormat();

    // Create test image, if enabled.
    VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = NULL;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = imageFormat;
    imageCreateInfo.extent = { imageWidth, imageHeight, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.queueFamilyIndexCount = 1;
    const uint32_t queueFamilyIndices = m_videoRenderer->m_vkDevCtx->GetGfxQueueFamilyIdx();
    imageCreateInfo.pQueueFamilyIndices = &queueFamilyIndices;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imageCreateInfo.flags = 0;
    m_videoRenderer->m_testFrameImage.CreateImage(m_videoRenderer->m_vkDevCtx, &imageCreateInfo,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                  1 /* ColorPatternColorBars */);

    // Create per a frame draw context num == mSwapchainNumBufs.

    const static VkSamplerYcbcrConversionCreateInfo defaultSamplerYcbcrConversionCreateInfo = {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        NULL,
        imageFormat,
        m_samplerYcbcrModelConversion,
        m_samplerYcbcrRange,
        { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY },
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_FILTER_LINEAR,
        false
    };

    // Create Vulkan's RenderPass
    VkResult result = m_videoRenderer->m_renderPass.CreateRenderPass(m_videoRenderer->m_vkDevCtx, ctx.format.format);
    if (result != VK_SUCCESS) {
        assert(!"ERROR: Could't create RenderPass!");
        return -1;
    }

    result =  m_videoRenderer->m_renderInfo.CreatePerDrawContexts(m_videoRenderer->m_vkDevCtx,
                                                                  ctx.swapchain,
                                                                  &ctx.extent,
                                                                  &m_viewport,
                                                                  &m_scissor,
                                                                  &ctx.format,
                                                                  m_videoRenderer->m_renderPass.getRenderPass(),
                                                                  &defaultSamplerInfo,
                                                                  &defaultSamplerYcbcrConversionCreateInfo);
    if (result != VK_SUCCESS) {
        assert(!"ERROR: Could't create rawContexts!");
        return -1;
    }
    return 0;
}

template<class FrameDataType>
void VulkanFrame<FrameDataType>::DetachSwapchain() { }

template<class FrameDataType>
void VulkanFrame<FrameDataType>::PrepareViewport(const VkExtent2D& extent)
{
    this->m_extent = extent;

    m_viewport.x = 0.0f;
    m_viewport.y = 0.0f;
    m_viewport.width = static_cast<float>(extent.width);
    m_viewport.height = static_cast<float>(extent.height);
    m_viewport.minDepth = 0.0f;
    m_viewport.maxDepth = 1.0f;

    m_scissor.offset = { 0, 0 };
    m_scissor.extent = extent;
}

template<class FrameDataType>
bool VulkanFrame<FrameDataType>::OnKey(Key key)
{
    switch (key) {
    case KEY_SHUTDOWN:
    case KEY_ESC:
        return false;
        break;
    case KEY_UP:
    case KEY_PAGE_UP:
        break;
    case KEY_DOWN:
    case KEY_PAGE_DOWN:
        break;
    case KEY_LEFT:
        break;
    case KEY_RIGHT:
        break;
    case KEY_SPACE:
        m_codecPaused = !m_codecPaused;
        break;
    default:
        break;
    }
    return true;
}

template<class FrameDataType>
bool VulkanFrame<FrameDataType>::OnFrame( int32_t renderIndex,
                          uint32_t           waitSemaphoreCount,
                          const VkSemaphore* pWaitSemaphores,
                          uint32_t           signalSemaphoreCount,
                          const VkSemaphore* pSignalSemaphores)
{
    bool continueLoop = true;
    const bool dumpDebug = false;
    const bool trainFrame = (renderIndex < 0);
    const bool gfxRendererIsEnabled = (m_videoRenderer != nullptr);
    m_frameCount++;

#if !defined(VK_VIDEO_NO_STDOUT_INFO)
    if (dumpDebug == false) {
        bool displayTimeNow = false;
        float fps = GetFrameRateFps(displayTimeNow);
        if (displayTimeNow) {
            std::cout << "\t\tFrame " << m_frameCount << ", FPS: " << fps << std::endl;
        }
    } else {
        uint64_t timeDiffNanoSec = GetTimeDiffNanoseconds();
        std::cout << "\t\t Time nanoseconds: " << timeDiffNanoSec <<
                     " milliseconds: " << timeDiffNanoSec / 1000 <<
                     " rate: " << 1000000000.0 / timeDiffNanoSec << std::endl;
    }
#endif

    FrameDataType& data = m_frameData[m_frameDataIndex];
    FrameDataType* pLastDecodedFrame = nullptr;

    if ((m_videoQueue->GetWidth() > 0) && !trainFrame) {

        pLastDecodedFrame = &data;

        // Graphics and present stages are not enabled.
        // Make sure the frame complete query or fence are signaled (video frame is processed) before returning the frame.
        if (false && (gfxRendererIsEnabled == false) && (pLastDecodedFrame != nullptr)) {

            if (pLastDecodedFrame->queryPool != VK_NULL_HANDLE) {
                auto startTime = std::chrono::steady_clock::now();
                VkQueryResultStatusKHR decodeStatus;
                VkResult result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx,
                                                 pLastDecodedFrame->queryPool,
                                                 pLastDecodedFrame->startQueryId,
                                                 1,
                                                 sizeof(decodeStatus),
                                                 &decodeStatus,
                                                 sizeof(decodeStatus),
                                                 VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

                assert(result == VK_SUCCESS);
                assert(decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);
                if ((result != VK_SUCCESS) || (decodeStatus != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)) {
                    fprintf(stderr, "\nERROR: GetQueryPoolResults() result: 0x%x\n", result);
                    return false;
                }

                auto deltaTime = std::chrono::steady_clock::now() - startTime;
                auto diffMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deltaTime);
                auto diffMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(deltaTime);
                if (dumpDebug) {
                    std::cout << pLastDecodedFrame->pictureIndex << ": frameWaitTime: " <<
                                 diffMilliseconds.count() << "." << diffMicroseconds.count() << " mSec" << std::endl;
                }
            } else if (pLastDecodedFrame->frameCompleteFence != VkFence()) {
                VkResult result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &pLastDecodedFrame->frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x\n", result);
                }
                result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, pLastDecodedFrame->frameCompleteFence);
                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "\nERROR: GetFenceStatus() result: 0x%x\n", result);
                }
            }
        }

        m_videoQueue->ReleaseFrame(pLastDecodedFrame);

        pLastDecodedFrame->Reset();

        bool endOfStream = false;
        int32_t numVideoFrames = 0;

        numVideoFrames = m_videoQueue->GetNextFrame(pLastDecodedFrame, &endOfStream);
        if (endOfStream && (numVideoFrames < 0)) {
            continueLoop = false;
#if !defined(VK_VIDEO_NO_STDOUT_INFO)
            bool displayTimeNow = true;
            float fps = GetFrameRateFps(displayTimeNow);
            if (displayTimeNow) {
                std::cout << "\t\tFrame " << m_frameCount << ", FPS: " << fps << std::endl;
            }
#endif
        }
    }

    // wait for the last submission since we reuse frame data
    if (dumpDebug && pLastDecodedFrame) {

        VkSharedBaseObj<VkImageResourceView> imageResourceView;
        pLastDecodedFrame->imageViews[FrameDataType::IMAGE_VIEW_TYPE_OPTIMAL_DISPLAY].GetImageResourceView(imageResourceView);

        std::cout << "<= Wait on picIdx: " << pLastDecodedFrame->pictureIndex
                  << "\t\tdisplayWidth: " << pLastDecodedFrame->displayWidth
                  << "\t\tdisplayHeight: " << pLastDecodedFrame->displayHeight
                  << "\t\tdisplayOrder: " << pLastDecodedFrame->displayOrder
                  << "\tdecodeOrder: " << pLastDecodedFrame->decodeOrder
                  << "\ttimestamp " << pLastDecodedFrame->timestamp
                  << "\tdstImageView " << (imageResourceView ? imageResourceView->GetImageResource()->GetImage() : VkImage())
                  << std::endl;
    }

    if (gfxRendererIsEnabled == false) {

        m_frameDataIndex = (m_frameDataIndex + 1) % m_frameData.size();
        return continueLoop;
    }

    VkResult result = DrawFrame(renderIndex,
                                waitSemaphoreCount,
                                pWaitSemaphores,
                                signalSemaphoreCount,
                                pSignalSemaphores,
                                pLastDecodedFrame);


    if (VK_SUCCESS != result) {
        return false;
    }

    return continueLoop;
}

template<class FrameDataType>
VkResult VulkanFrame<FrameDataType>::DrawFrame( int32_t            renderIndex,
                                                uint32_t           waitSemaphoreCount,
                                                const VkSemaphore* pWaitSemaphores,
                                                uint32_t           signalSemaphoreCount,
                                                const VkSemaphore* pSignalSemaphores,
                                                FrameDataType*     inFrame)
{
    const bool dumpDebug = false;
    if (renderIndex < 0) {
        renderIndex = -renderIndex;
    }

    vulkanVideoUtils::VulkanPerDrawContext* pPerDrawContext = m_videoRenderer->m_renderInfo.GetDrawContext(renderIndex);

    VkSharedBaseObj<VkImageResourceView> imageResourceView;
    inFrame->imageViews[FrameDataType::IMAGE_VIEW_TYPE_OPTIMAL_DISPLAY].GetImageResourceView(imageResourceView);

    bool doTestPatternFrame = ((inFrame == NULL) ||
                               (!imageResourceView ||
                                (imageResourceView->GetImageView() == VK_NULL_HANDLE)) ||
                               m_videoRenderer->m_useTestImage);

    VkImageResourceView* pView = inFrame ? imageResourceView : (VkImageResourceView*)nullptr;
    vulkanVideoUtils::ImageResourceInfo rtImage(pView, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR);
    const vulkanVideoUtils::ImageResourceInfo* pRtImage = doTestPatternFrame ? &m_videoRenderer->m_testFrameImage : &rtImage;
    VkFence frameConsumerDoneFence = doTestPatternFrame ? VkFence() : inFrame->frameConsumerDoneFence;
    int32_t displayWidth  = doTestPatternFrame ? pRtImage->imageWidth  : inFrame->displayWidth;
    int32_t displayHeight = doTestPatternFrame ? pRtImage->imageHeight : inFrame->displayHeight;
    VkFormat imageFormat  = doTestPatternFrame ? pRtImage->imageFormat : rtImage.imageFormat;

    if (pPerDrawContext->samplerYcbcrConversion.GetSamplerYcbcrConversionCreateInfo().format != imageFormat) {

        const static VkSamplerYcbcrConversionCreateInfo newSamplerYcbcrConversionCreateInfo = {
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            NULL,
            imageFormat,
            m_samplerYcbcrModelConversion, // FIXME: consider/update the ITU 601, 709, 2020
            m_samplerYcbcrRange,           // FIXME: consider/update the ITU range
#ifndef NV_RMAPI_TEGRA
            { VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY },
#else
            { VK_COMPONENT_SWIZZLE_B,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_R,
                VK_COMPONENT_SWIZZLE_IDENTITY },
#endif
            VK_CHROMA_LOCATION_MIDPOINT,
            VK_CHROMA_LOCATION_MIDPOINT,
            VK_FILTER_LINEAR,
            false
        };

        if (pPerDrawContext->samplerYcbcrConversion.SamplerRequiresUpdate(nullptr, &newSamplerYcbcrConversionCreateInfo)) {
            m_videoRenderer->m_renderInfo.UpdatePerDrawContexts(pPerDrawContext, &m_viewport, &m_scissor,
                m_videoRenderer->m_renderPass.getRenderPass(), &defaultSamplerInfo,
                &newSamplerYcbcrConversionCreateInfo);
        }
    }

    if (pPerDrawContext->descriptorSetLayoutBinding.GetDescriptorSetLayoutInfo().GetDescriptorLayoutMode() == 0) {

        VkWriteDescriptorSet writeDescriptorSet{};

        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = VK_NULL_HANDLE;
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = (pPerDrawContext->samplerYcbcrConversion.GetSampler() != VK_NULL_HANDLE) ?
                                                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

        VkDescriptorImageInfo imageDescriptor{};
        imageDescriptor.sampler = pPerDrawContext->samplerYcbcrConversion.GetSampler();
        imageDescriptor.imageView = pRtImage->view;
        assert(imageDescriptor.imageView);
        imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writeDescriptorSet.pImageInfo = &imageDescriptor; // RGBA or Sampled YCbCr

        pPerDrawContext->descriptorSetLayoutBinding.WriteDescriptorSet(1, &writeDescriptorSet);
    }

    pPerDrawContext->RecordCommandBuffer(*pPerDrawContext->commandBuffer.GetCommandBuffer(),
                                         m_videoRenderer->m_renderPass.getRenderPass(),
                                         pRtImage,
                                         displayWidth, displayHeight,
                                         pPerDrawContext->frameBuffer.GetFbImage(),
                                         pPerDrawContext->frameBuffer.GetFrameBuffer(), &m_scissor,
                                         pPerDrawContext->gfxPipeline.getPipeline(),
                                         pPerDrawContext->descriptorSetLayoutBinding,
                                         pPerDrawContext->samplerYcbcrConversion,
                                         m_videoRenderer->m_vertexBuffer);

    if (dumpDebug) {
        std::cout << "Drawing Frame " << m_frameCount << " FB: " << renderIndex << std::endl;
    }

    if (dumpDebug && inFrame) {
        std::cout << "<= Present picIdx: " << inFrame->pictureIndex
                  << "\t\tdisplayOrder: " << inFrame->displayOrder
                  << "\tdecodeOrder: " << inFrame->decodeOrder
                  << "\ttimestamp " << inFrame->timestamp
                  << pRtImage->view << std::endl;
    }

    VkResult result = VK_SUCCESS;
    if (!m_videoRenderer->m_useTestImage && inFrame) {
        if (inFrame->frameCompleteSemaphore == VkSemaphore()) {
            if (inFrame->frameCompleteFence == VkFence()) {
                VkQueue videoDecodeQueue = m_vkDevCtx->GetVideoDecodeQueue();
                if (videoDecodeQueue != VkQueue()) {
                    result = m_vkDevCtx->QueueWaitIdle(videoDecodeQueue);
                    assert(result == VK_SUCCESS);
                    if (result != VK_SUCCESS) {
                        fprintf(stderr, "\nERROR: QueueWaitIdle() result: 0x%x\n", result);
                    }
                }
            } else {
                result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &inFrame->frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x\n", result);
                }
                result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, inFrame->frameCompleteFence);
                assert(result == VK_SUCCESS);
                if (result != VK_SUCCESS) {
                    fprintf(stderr, "\nERROR: GetFenceStatus() result: 0x%x\n", result);
                }
            }
        }
    }

    // For queryPool debugging
    bool getDecodeStatusBeforePresent = false;
    if (getDecodeStatusBeforePresent && (inFrame != nullptr) &&
            (inFrame->queryPool != VK_NULL_HANDLE) &&
            (inFrame->startQueryId >= 0) &&
            (inFrame->numQueries > 0)) {

        if (inFrame->frameCompleteFence != VkFence()) {
            result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &inFrame->frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x\n", result);
            }
            result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, inFrame->frameCompleteFence);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: GetFenceStatus() result: 0x%x\n", result);
            }
        }

        VkQueryResultStatusKHR decodeStatus;
        result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx,
                                                 inFrame->queryPool,
                                                 inFrame->startQueryId,
                                                 1,
                                                 sizeof(decodeStatus),
                                                 &decodeStatus,
                                                 sizeof(decodeStatus),
                                                 VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: GetQueryPoolResults() result: 0x%x\n", result);
        }
        assert(decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

        if (dumpDebug) {
            std::cout << "\t +++++++++++++++++++++++++++< " << (inFrame ? inFrame->pictureIndex : -1)
                      << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << (inFrame ? inFrame->pictureIndex : -1) << std::endl
                      << "\t\tdecodeStatus: " << decodeStatus << std::endl;
        }
    }

    const uint32_t waitSemaphoreMaxCount = 2;
    VkSemaphoreSubmitInfoKHR waitSemaphoreInfos[waitSemaphoreMaxCount]{};

    const uint32_t signalSemaphoreMaxCount = 2;
    VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[signalSemaphoreMaxCount]{};

    for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
        waitSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[i].pNext = nullptr;
        waitSemaphoreInfos[i].semaphore = pWaitSemaphores[i];
        waitSemaphoreInfos[i].value = 0; // Binary semaphore
        waitSemaphoreInfos[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waitSemaphoreInfos[i].deviceIndex = 0;
    }

    for (uint32_t i = 0; i < signalSemaphoreCount; i++) {
        signalSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[i].pNext = nullptr;
        signalSemaphoreInfos[i].semaphore = pSignalSemaphores[i];
        signalSemaphoreInfos[i].value = 0; // Binary semaphore
        signalSemaphoreInfos[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signalSemaphoreInfos[i].deviceIndex = 0;
    }

    if (inFrame && (inFrame->frameCompleteSemaphore != VK_NULL_HANDLE)) {

        waitSemaphoreInfos[waitSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].pNext = nullptr;
        waitSemaphoreInfos[waitSemaphoreCount].semaphore = inFrame->frameCompleteSemaphore;
        waitSemaphoreInfos[waitSemaphoreCount].value =     inFrame->frameCompleteDoneSemValue;
        waitSemaphoreInfos[waitSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR |
                                                           VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR |
                                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
        waitSemaphoreInfos[waitSemaphoreCount].deviceIndex = 0;
        waitSemaphoreCount++;

        signalSemaphoreInfos[signalSemaphoreCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
        signalSemaphoreInfos[signalSemaphoreCount].pNext = nullptr;
        signalSemaphoreInfos[signalSemaphoreCount].semaphore = inFrame->consumerCompleteSemaphore;
        signalSemaphoreInfos[signalSemaphoreCount].value     = inFrame->frameConsumerDoneSemValue;
        signalSemaphoreInfos[signalSemaphoreCount].stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        signalSemaphoreInfos[signalSemaphoreCount].deviceIndex = 0;
        signalSemaphoreCount++;

        inFrame->hasConsummerSignalSemaphore = true;
    }

    assert(waitSemaphoreCount <= waitSemaphoreMaxCount);
    assert(signalSemaphoreCount <= signalSemaphoreMaxCount);

    if (frameConsumerDoneFence != VkFence()) {
        inFrame->hasConsummerSignalFence = true;
    }

    VkCommandBufferSubmitInfoKHR cmdBufferInfos;
    cmdBufferInfos.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR;
    cmdBufferInfos.pNext = nullptr;
    cmdBufferInfos.commandBuffer = *pPerDrawContext->commandBuffer.GetCommandBuffer();
    cmdBufferInfos.deviceMask = 0;

    // Submit info
    VkSubmitInfo2KHR submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR;
    submitInfo.pNext = nullptr;
    submitInfo.flags = 0;
    submitInfo.waitSemaphoreInfoCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphoreInfos = waitSemaphoreInfos;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdBufferInfos;
    submitInfo.signalSemaphoreInfoCount = signalSemaphoreCount;
    submitInfo.pSignalSemaphoreInfos = signalSemaphoreInfos;

    // For fence/sync debugging
    if (false && inFrame && inFrame->frameCompleteFence) {
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &inFrame->frameCompleteFence, true, 100 * 1000 * 1000);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x\n", result);
        }
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, inFrame->frameCompleteFence);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: GetFenceStatus() result: 0x%x\n", result);
        }
    }

    result = m_vkDevCtx->MultiThreadedQueueSubmit(VulkanDeviceContext::GRAPHICS,
                                                  0, // queueIndex
                                                  1, // submitCount
                                                  &submitInfo,
                                                  frameConsumerDoneFence,
                                                  "Graphics Submit",
                                                  (inFrame != nullptr) ? inFrame->decodeOrder  : UINT64_MAX,
                                                  (inFrame != nullptr) ? inFrame->displayOrder : UINT64_MAX);
    if (result != VK_SUCCESS) {
        assert(result == VK_SUCCESS);
        fprintf(stderr, "\nERROR: MultiThreadedQueueSubmit() result: 0x%x\n", result);
        return result;
    }

    if (false && (frameConsumerDoneFence != VkFence())) { // For fence/sync debugging
        const uint64_t fenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameConsumerDoneFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x\n", result);
        }
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: GetFenceStatus() result: 0x%x\n", result);
        }
    }

#if 0 // for testing VK_KHR_external_fence_fd
    int fd = -1; // VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    const VkFenceGetFdInfoKHR getFdInfo =  { VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, NULL, data.lastDecodedFrame.frameConsumerDoneFence, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    res = m_vkDevCtx->GetFenceFdKHR(*m_vkDevCtx, &getFdInfo, &fd);
    close(fd);
#endif

    m_frameDataIndex = (m_frameDataIndex + 1) % m_frameData.size();

    if (false) {
        // Add a 20ms sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return result;
}

template<class FrameDataType>
VkResult VulkanFrame<FrameDataType>::Create(const VulkanDeviceContext* vkDevCtx,
                                            VkSharedBaseObj<VulkanFrame>& vulkanFrame)
{
    VkSharedBaseObj<VulkanFrame> vkVideoFrame(new VulkanFrame<FrameDataType>(vkDevCtx));

    if (vkVideoFrame) {
        vulkanFrame = vkVideoFrame;
        return VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

#include "VulkanDecoderFrameProcessor.h"

VkResult CreateDecoderFrameProcessor(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<FrameProcessor>& frameProcessor)
{

    VkSharedBaseObj<VulkanFrame<VulkanDecodedFrame>> vulkanFrame;
    VkResult result = VulkanFrame<VulkanDecodedFrame>::Create(vkDevCtx, vulkanFrame);
    if (result != VK_SUCCESS) {
        return result;
    }

    frameProcessor = vulkanFrame;
    return result;
}

VkResult DecoderFrameProcessorState::Init(const VulkanDeviceContext* vkDevCtx,
                                          VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>>& videoQueue,
                                          int32_t maxNumberOfFrames)
{
    VkResult result = CreateDecoderFrameProcessor(vkDevCtx, m_frameProcessor);

    if (result != VK_SUCCESS) {
        return result;
    }

    if ((maxNumberOfFrames > 0 ) && (m_frameProcessor != nullptr)) {

        int32_t ret = m_frameProcessor->CreateFrameData(maxNumberOfFrames);
        assert(ret == maxNumberOfFrames);
        if (ret != maxNumberOfFrames) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        m_maxNumberOfFrames = maxNumberOfFrames;
    }

    VkSharedBaseObj<VkVideoRefCountBase> _videoQueue(videoQueue);
    m_frameProcessor->AttachQueue(_videoQueue);

    return result;
}

void DecoderFrameProcessorState::Deinit()
{
    if (m_maxNumberOfFrames > 0) {
        m_frameProcessor->DestroyFrameData();
        m_maxNumberOfFrames = 0;
    }
}

#include "VkCodecUtils/VulkanEncoderFrameProcessor.h"

VkResult CreateEncoderFrameProcessor(const VulkanDeviceContext* vkDevCtx,
                                     VkSharedBaseObj<FrameProcessor>& frameProcessor)
{

    VkSharedBaseObj<VulkanFrame<VulkanEncoderInputFrame>> vulkanFrame;
    VkResult result = VulkanFrame<VulkanEncoderInputFrame>::Create(vkDevCtx, vulkanFrame);
    if (result != VK_SUCCESS) {
        return result;
    }

    frameProcessor = vulkanFrame;
    return result;
}
