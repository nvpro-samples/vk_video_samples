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
#include <atomic>

#include "VkCodecUtils/FrameProcessorFactory.h"

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkShell/Shell.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VulkanFrame.h"
#include "vk_enum_string_helper.h"
#include <NvCodecUtils/Logger.h>
#include "NvCodecUtils/VideoStreamDemuxer.h"

#include "VkVideoDecoder/VkVideoDecoder.h"

// Vulkan call wrapper
#define CALL_VK(func)                                             \
    if (VK_SUCCESS != (func)) {                                   \
        LOG(ERROR) << "VulkanVideoFrame: "                        \
                   << "File " << __FILE__ << "line " << __LINE__; \
        assert(false);                                            \
    }

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

VulkanFrame::VulkanFrame(const ProgramConfig& programConfig,
                         const VulkanDeviceContext* vkDevCtx,
                         VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor)
    : FrameProcessor(programConfig)
    , m_refCount(0)
    , m_vkDevCtx(vkDevCtx)
    , m_videoProcessor(videoProcessor)
    , m_samplerYcbcrModelConversion(VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709)
    , m_samplerYcbcrRange(VK_SAMPLER_YCBCR_RANGE_ITU_NARROW)
    , m_videoRenderer(nullptr)
    , m_lastRealTimeNsecs(0)
    , m_codecPaused(false)
    , m_gfxQueue()
    , m_vkFormat()
    , m_physicalDevProps()
    , m_frameData()
    , m_frameDataIndex()
{
}

VulkanFrame::~VulkanFrame()
{
    DetachShell();
}

int VulkanFrame::GetVideoWidth() { return m_videoProcessor->IsValid() ? m_videoProcessor->GetWidth() : m_scissor.extent.width; }

int VulkanFrame::GetVideoHeight() { return m_videoProcessor->IsValid() ? m_videoProcessor->GetHeight() : m_scissor.extent.height; }

int VulkanFrame::AttachShell(const Shell& sh)
{
    const Shell::Context& ctx = sh.GetContext();
    m_gfxQueue = ctx.devCtx->GetGfxQueue();

    m_vkDevCtx->GetPhysicalDeviceProperties(ctx.devCtx->getPhysicalDevice(), &m_physicalDevProps);

    const uint32_t apiMajorVersion = VK_API_VERSION_MAJOR(m_physicalDevProps.apiVersion);
    const uint32_t apiMinorVersion = VK_API_VERSION_MINOR(m_physicalDevProps.apiVersion);
    const uint32_t apiPatchVersion = VK_API_VERSION_PATCH(m_physicalDevProps.apiVersion);

    if (m_physicalDevProps.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 199)) {
        std::cerr << std::endl << "Incompatible Vulkan API version: " << apiMajorVersion << "." << apiMinorVersion << "." << apiPatchVersion << std::endl;
        std::cerr << "Info: Driver version is: " << m_physicalDevProps.driverVersion << std::endl;;
        std::cerr << "Please upgrade your driver. The version supported is: 1.2.199 or later aka " << std::hex << VK_MAKE_API_VERSION(0, 1, 2, 199) << std::endl;
        assert(!"Incompatible API version - please upgrade your driver.");
        return -1;
    }

    VkQueue videoDecodeQueue = m_vkDevCtx->GetVideoDecodeQueue();
    const bool useTestImage = (videoDecodeQueue == VkQueue());
    m_videoRenderer = new vulkanVideoUtils::VkVideoAppCtx(useTestImage);
    if (m_videoRenderer == nullptr) {
        return -1;
    }

    m_videoRenderer->m_vkDevCtx = m_vkDevCtx;

    m_vkFormat = ctx.format.format;

    CreateFrameData((int)ctx.backBuffers.size());

    // Create Vulkan's Vertex buffer
    // position/texture coordinate pair per vertex.
    static const vulkanVideoUtils::Vertex vertices[4] = {
       //    Vertex         Texture coordinate
        { {  1.0f,  1.0f }, { 1.0f, 1.0f }, },
        { { -1.0f,  1.0f }, { 0.0f, 1.0f }, },
        { { -1.0f, -1.0f }, { 0.0f, 0.0f }, },
        { {  1.0f, -1.0f }, { 1.0f, 0.0f }, },

    };

    CALL_VK(m_videoRenderer->m_vertexBuffer.CreateVertexBuffer(m_videoRenderer->m_vkDevCtx,
                                                               (const float*)vertices, sizeof(vertices),
                                                               sizeof(vertices) / sizeof(vertices[0])));

    return 0;
}

void VulkanFrame::DetachShell()
{
    DestroyFrameData();

    delete m_videoRenderer;
    m_videoRenderer = nullptr;
}

int VulkanFrame::CreateFrameData(int count)
{
    m_frameData.resize(count);

    m_frameDataIndex = 0;

    for (auto& data : m_frameData) {
        data.lastDecodedFrame.Reset();
    }

    if (m_frameData.size() >= (size_t)count) {
        return count;
    }

    return -1;
}

void VulkanFrame::DestroyFrameData()
{
    for (auto& data : m_frameData) {
        data.lastDecodedFrame.Reset();
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

int VulkanFrame::AttachSwapchain(const Shell& sh)
{
    const Shell::Context& ctx = sh.GetContext();

    PrepareViewport(ctx.extent);

    uint32_t imageWidth = GetVideoWidth();
    uint32_t imageHeight = GetVideoHeight();

    // Create test image, if enabled.
    VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = NULL;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = m_videoProcessor->GetFrameImageFormat();
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
        m_videoProcessor->GetFrameImageFormat(),
        m_samplerYcbcrModelConversion,
        m_samplerYcbcrRange,
        { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY },
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_FILTER_NEAREST,
        false
    };

    // Create Vulkan's RenderPass
    m_videoRenderer->m_renderPass.CreateRenderPass(m_videoRenderer->m_vkDevCtx, ctx.format.format);

    m_videoRenderer->m_renderInfo.CreatePerDrawContexts(m_videoRenderer->m_vkDevCtx,
                                                       ctx.swapchain, &ctx.extent, &m_viewport, &m_scissor,
        &ctx.format, m_videoRenderer->m_renderPass.getRenderPass(), &defaultSamplerInfo,
        &defaultSamplerYcbcrConversionCreateInfo);

    return 0;
}

void VulkanFrame::DetachSwapchain() { }

void VulkanFrame::PrepareViewport(const VkExtent2D& extent)
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

bool VulkanFrame::OnKey(Key key)
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

bool VulkanFrame::OnFrame( int32_t           renderIndex,
                          uint32_t           waitSemaphoreCount,
                          const VkSemaphore* pWaitSemaphores,
                          uint32_t           signalSemaphoreCount,
                          const VkSemaphore* pSignalSemaphores,
                          const DecodedFrame** ppOutFrame)
{
    bool continueLoop = true;
    const bool dumpDebug = false;
    const bool trainFrame = (renderIndex < 0);
    const bool gfxRendererIsEnabled = (m_videoRenderer != nullptr);
    m_frameCount++;

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

    FrameData& data = m_frameData[m_frameDataIndex];
    DecodedFrame* pLastDecodedFrame = NULL;

    if (m_videoProcessor->IsValid() && !trainFrame) {

        pLastDecodedFrame = &data.lastDecodedFrame;

        // Graphics and present stages are not enabled.
        // Make sure the frame complete query or fence are signaled (video frame is processed) before returning the frame.
        if ((gfxRendererIsEnabled == false) && (pLastDecodedFrame != nullptr)) {

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

        m_videoProcessor->ReleaseDisplayedFrame(pLastDecodedFrame);

        pLastDecodedFrame->Reset();

        bool endOfStream = false;
        int32_t numVideoFrames = 0;

        numVideoFrames = m_videoProcessor->GetNextFrame(pLastDecodedFrame, &endOfStream);
        if (endOfStream && (numVideoFrames < 0)) {
            continueLoop = false;
            bool displayTimeNow = true;
            float fps = GetFrameRateFps(displayTimeNow);
            if (displayTimeNow) {
                std::cout << "\t\tFrame " << m_frameCount << ", FPS: " << fps << std::endl;
            }
        }
    }

    // wait for the last submission since we reuse frame data
    if (dumpDebug && pLastDecodedFrame) {
        std::cout << "<= Wait on picIdx: " << pLastDecodedFrame->pictureIndex
                  << "\t\tdisplayWidth: " << pLastDecodedFrame->displayWidth
                  << "\t\tdisplayHeight: " << pLastDecodedFrame->displayHeight
                  << "\t\tdisplayOrder: " << pLastDecodedFrame->displayOrder
                  << "\tdecodeOrder: " << pLastDecodedFrame->decodeOrder
                  << "\ttimestamp " << pLastDecodedFrame->timestamp
                  << "\tdstImageView " << (pLastDecodedFrame->outputImageView ?
                          pLastDecodedFrame->outputImageView->GetImageResource()->GetImage() : VkImage())
                  << std::endl;
    }

    if (gfxRendererIsEnabled == false) {

        if (ppOutFrame) {
            *ppOutFrame = pLastDecodedFrame;
        }

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


VkResult VulkanFrame::DrawFrame( int32_t           renderIndex,
                                uint32_t           waitSemaphoreCount,
                                const VkSemaphore* pWaitSemaphores,
                                uint32_t           signalSemaphoreCount,
                                const VkSemaphore* pSignalSemaphores,
                                DecodedFrame*      inFrame)
{
    const bool dumpDebug = false;
    if (renderIndex < 0) {
        renderIndex = -renderIndex;
    }
    vulkanVideoUtils::VulkanPerDrawContext* pPerDrawContext = m_videoRenderer->m_renderInfo.GetDrawContext(renderIndex);

    bool doTestPatternFrame = ((inFrame == NULL) ||
                               (!inFrame->outputImageView ||
                                (inFrame->outputImageView->GetImageView() == VK_NULL_HANDLE)) ||
                               m_videoRenderer->m_useTestImage);

    vulkanVideoUtils::ImageResourceInfo rtImage(inFrame->outputImageView, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR);
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
            VK_FILTER_NEAREST,
            false
        };

        if (pPerDrawContext->samplerYcbcrConversion.SamplerRequiresUpdate(nullptr, &newSamplerYcbcrConversionCreateInfo)) {
            m_videoRenderer->m_renderInfo.UpdatePerDrawContexts(pPerDrawContext, &m_viewport, &m_scissor,
                m_videoRenderer->m_renderPass.getRenderPass(), &defaultSamplerInfo,
                &newSamplerYcbcrConversionCreateInfo);
        }
    }

    if (pPerDrawContext->descriptorSetLayoutBinding.GetDescriptorLayoutMode() == 0) {
        pPerDrawContext->descriptorSetLayoutBinding.WriteDescriptorSet(VkSampler(0), pRtImage->view);
    }

    pPerDrawContext->commandBuffer.CreateCommandBuffer(
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
        LOG(INFO) << "Drawing Frame " << m_frameCount << " FB: " << renderIndex << std::endl;
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

    //For queryPool debugging
    bool getDecodeStatusBeforePresent = false;
    if (getDecodeStatusBeforePresent && (inFrame != nullptr) &&
            (inFrame->queryPool != VK_NULL_HANDLE) &&
            (inFrame->startQueryId >= 0) &&
            (inFrame->numQueries > 0)) {

        if (inFrame->frameCompleteFence != VkFence()) {
            result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &inFrame->frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: WaitForFences() result: 0x%x (%s)\n", result, string_VkResult(result));
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

    const uint32_t maxWaitSemaphores = 2;
    uint32_t numWaitSemaphores = 0;
    VkSemaphore waitSemaphores[maxWaitSemaphores] = {};

    assert(waitSemaphoreCount <= 1);
    if ((waitSemaphoreCount > 0) && (pWaitSemaphores != nullptr)) {
        waitSemaphores[numWaitSemaphores++] = *pWaitSemaphores;
    }

    if (inFrame && (inFrame->frameCompleteSemaphore != VkSemaphore())) {
        waitSemaphores[numWaitSemaphores++] = inFrame->frameCompleteSemaphore;
    }
    assert(numWaitSemaphores <= maxWaitSemaphores);

    const uint32_t maxSignalSemaphores = 2;
    uint32_t numSignalSemaphores = 0;
    VkSemaphore signalSemaphores[maxSignalSemaphores] = {};

    assert(signalSemaphoreCount <= 1);
    if ((signalSemaphoreCount > 0) && (pSignalSemaphores != nullptr)) {
        signalSemaphores[numSignalSemaphores++] = *pSignalSemaphores;
    }

    if (inFrame && (inFrame->frameConsumerDoneSemaphore != VkSemaphore())) {
        signalSemaphores[numSignalSemaphores++] = inFrame->frameConsumerDoneSemaphore;
        inFrame->hasConsummerSignalSemaphore = true;
    }
    assert(numSignalSemaphores <= maxSignalSemaphores);

    if (frameConsumerDoneFence != VkFence()) {
        inFrame->hasConsummerSignalFence = true;
    }


    // Wait for the image to be owned and signal for render completion
    VkPipelineStageFlags primaryCmdSubmitWaitStages[2] = { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    VkSubmitInfo primaryCmdSubmitInfo = VkSubmitInfo();
    primaryCmdSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    primaryCmdSubmitInfo.pWaitDstStageMask = primaryCmdSubmitWaitStages;
    primaryCmdSubmitInfo.commandBufferCount = 1;

    primaryCmdSubmitInfo.waitSemaphoreCount = numWaitSemaphores;
    primaryCmdSubmitInfo.pWaitSemaphores = numWaitSemaphores ? waitSemaphores : NULL;
    primaryCmdSubmitInfo.pCommandBuffers = pPerDrawContext->commandBuffer.getCommandBuffer();

    primaryCmdSubmitInfo.signalSemaphoreCount = numSignalSemaphores;
    primaryCmdSubmitInfo.pSignalSemaphores = numSignalSemaphores ? signalSemaphores : NULL;

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

    result = m_vkDevCtx->QueueSubmit(m_gfxQueue, 1, &primaryCmdSubmitInfo, frameConsumerDoneFence);
    if (result != VK_SUCCESS) {
        assert(result == VK_SUCCESS);
        fprintf(stderr, "\nERROR: QueueSubmit() result: 0x%x\n", result);
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

    return result;
}

VkResult VulkanFrame::Create(const ProgramConfig& programConfig,
                             const VulkanDeviceContext* vkDevCtx,
                             VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor,
                             VkSharedBaseObj<VulkanFrame>& vulkanFrame)
{
    VkSharedBaseObj<VulkanFrame> vkVideoFrame(new VulkanFrame(programConfig, vkDevCtx, videoProcessor));

    if (vkVideoFrame) {
        vulkanFrame = vkVideoFrame;
        return VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult CreateFrameProcessor(const ProgramConfig& programConfig,
                              const VulkanDeviceContext* vkDevCtx,
                              VkSharedBaseObj<VulkanVideoProcessor>& videoProcessor,
                              VkSharedBaseObj<FrameProcessor>& frameProcessor)
{

    VkSharedBaseObj<VulkanFrame> vulkanFrame;
    VkResult result = VulkanFrame::Create(programConfig, vkDevCtx, videoProcessor, vulkanFrame);
    if (result != VK_SUCCESS) {
        return result;
    }

    if (vulkanFrame) {
        std::ifstream validVideoFileStream(vulkanFrame->GetSettings().videoFileName, std::ifstream::in);
        if (!validVideoFileStream) {
            std::cerr << "Invalid input video file: " << vulkanFrame->GetSettings().videoFileName << std::endl;
            std::cerr << "Please provide a valid name for the input video file to be decoded with the \"-i\" command line option." << std::endl;
            std::cerr << "   vk-video-dec-test -i <absolute file path location>" << std::endl;

            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    frameProcessor = vulkanFrame;
    return result;
}
