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

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "VkShell/FrameProcessorFactory.h"

#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanVideoUtils.h"
#include "VkShell/Shell.h"
#include "VulkanFrame.h"

#include <NvCodecUtils/Logger.h>

#include "NvCodecUtils/FFmpegDemuxer.h"
#include "NvVkDecoder/NvVkDecoder.h"

// Vulkan call wrapper
#define CALL_VK(func)                                             \
    if (VK_SUCCESS != (func)) {                                   \
        LOG(ERROR) << "VulkanVideoFrame: "                        \
                   << "File " << __FILE__ << "line " << __LINE__; \
        assert(false);                                            \
    }

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#define CLOCK_MONOTONIC 0
extern int clock_gettime(int dummy, struct timespec* ct);
#endif

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

VulkanFrame::VulkanFrame(const std::vector<std::string>& args)
    : FrameProcessor("VulkanFrame", args)
    , frameImageFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
    , samplerYcbcrModelConversion(VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709)
    , samplerYcbcrRange(VK_SAMPLER_YCBCR_RANGE_ITU_NARROW)
    , lastVideoFormatUpdate(0)
    , pVideoRenderer(nullptr)
    , lastRealTimeNsecs(0)
    , multithread_(false)
    , use_push_constants_(false)
    , codec_paused_(false)
    , camera_(1)
    , frame_data_()
    , render_pass_clear_value_({ { { 0.0f, 0.1f, 0.2f, 1.0f } } })
    , m_videoProcessor()
{
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == "-s")
            multithread_ = false;
        else if (*it == "-p")
            use_push_constants_ = true;
    }

    for (auto it = 0; it < MAX_NUM_BUFFER_SLOTS; it++) {
        // frameImages_[MAX_NUM_BUFFER_SLOTS]
    }

    init_workers();
}

VulkanFrame::~VulkanFrame() { }

void VulkanFrame::init_workers() { }

int VulkanFrame::GetVideoWidth() { return m_videoProcessor ? m_videoProcessor.GetWidth() : scissor_.extent.width; }

int VulkanFrame::GetVideoHeight() { return m_videoProcessor ? m_videoProcessor.GetHeight() : scissor_.extent.height; }

int VulkanFrame::attach_shell(Shell& sh)
{
    FrameProcessor::attach_shell(sh);

    const Shell::Context& ctx = sh.context();
    queue_ = ctx.frameProcessor_queue;
    queue_family_ = ctx.frameProcessor_queue_family;

    vk::GetPhysicalDeviceProperties(ctx.physical_dev, &physical_dev_props_);
    VkPhysicalDeviceMemoryProperties mem_props;
    vk::GetPhysicalDeviceMemoryProperties(ctx.physical_dev, &mem_props);

    const bool useTestImage = (ctx.video_queue == VkQueue());
    pVideoRenderer = new vulkanVideoUtils::VkVideoAppCtx(useTestImage);
    if (pVideoRenderer == nullptr) {
        return -1;
    }

    pVideoRenderer->device_.AttachVulkanDevice(ctx.instance, ctx.physical_dev, ctx.dev, ctx.frameProcessor_queue_family,
        ctx.frameProcessor_queue, &mem_props);

    format_ = ctx.format.format;

    mem_flags_.reserve(mem_props.memoryTypeCount);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        mem_flags_.push_back(mem_props.memoryTypes[i].propertyFlags);

    create_frame_data((int)ctx.backBuffers_.size());

    // Create Vulkan's Vertex buffer
    // position/texture coordinate pair per vertex.
    static const vulkanVideoUtils::Vertex vertices[4] = { {
                                                              { 1.0f, 1.0f },
                                                              { 1.0f, 1.0f },
                                                          },
        {
            { -1.0f, 1.0f },
            { 0.0f, 1.0f },
        },
        {
            { -1.0f, -1.0f },
            { 0.0f, 0.0f },
        },
        {
            { 1.0f, -1.0f },
            { 1.0f, 0.0f },
        }

    };

    CALL_VK(pVideoRenderer->vertexBuffer_.CreateVertexBuffer(&pVideoRenderer->device_, (float*)vertices, sizeof(vertices),
        sizeof(vertices) / sizeof(vertices[0])));

    if (ctx.video_queue != VkQueue()) {
        const VulkanDecodeContext vulkanDecodeContext = { ctx.instance, ctx.physical_dev, ctx.dev, ctx.video_decode_queue_family,
            ctx.video_queue };

        const char* filePath = settings_.videoFileName.c_str();
        m_videoProcessor.Init(&vulkanDecodeContext, &pVideoRenderer->device_, filePath);

        frameImageFormat = m_videoProcessor.GetFrameImageFormat(&settings_.video_width, &settings_.video_height);
    }

    return 0;
}

void VulkanFrame::detach_shell()
{
    m_videoProcessor.Deinit();

    destroy_frame_data();

    delete pVideoRenderer;
    pVideoRenderer = nullptr;

    FrameProcessor::detach_shell();
}

void VulkanFrame::create_frame_data(int count)
{
    frame_data_.resize(count);

    frame_data_index_ = 0;

    for (auto& data : frame_data_) {
        memset(&data.lastDecodedFrame, 0x00, sizeof(data.lastDecodedFrame));
        data.lastDecodedFrame.pictureIndex = -1;
    }
}

void VulkanFrame::destroy_frame_data()
{
    frame_data_.clear();
}

static const VkSamplerCreateInfo defaultSamplerInfo = {
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    // mipLodBias  anisotropyEnable  maxAnisotropy  compareEnable      compareOp         minLod  maxLod          borderColor
    // unnormalizedCoordinates
    0.0, false, 0.00, false, VK_COMPARE_OP_NEVER, 0.0, 16.0, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, false
};

int VulkanFrame::attach_swapchain()
{
    const Shell::Context& ctx = shell_->context();

    prepare_viewport(ctx.extent);
    update_camera();

    // TODO: Use the codec queries for that purpose. Do not hard code the alignment.
    // Align the width to 64-bit
    // uint32_t alignedImageWidth  = ((GetVideoWidth()  + 63) & ~63);
    uint32_t alignedImageWidth = GetVideoWidth();
    // Align the height to 32-bit
    // uint32_t alignedImageHeight = ((GetVideoHeight() + 31) & ~31);
    uint32_t alignedImageHeight = GetVideoHeight();

    // assert(alignedImageWidth  == (((uint32_t)GetVideoWidth()  + 63) & ~63));
    // assert(alignedImageHeight == (((uint32_t)GetVideoHeight() + 31) & ~31));

    // Create test image, if enabled.
    VkImageCreateInfo imageCreateInfo = VkImageCreateInfo();
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = NULL;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = frameImageFormat;
    imageCreateInfo.extent = { alignedImageWidth, alignedImageHeight, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.queueFamilyIndexCount = 1;
    imageCreateInfo.pQueueFamilyIndices = &pVideoRenderer->device_.queueFamilyIndex_;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imageCreateInfo.flags = 0;
    pVideoRenderer->testFrameImage_.CreateImage(&pVideoRenderer->device_, &imageCreateInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        1 /* ColorPatternColorBars */, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);

    // Create per a frame draw context num == mSwapchainNumBufs.

    // TODO: Fix the parameters below based on the bitstream parameters.
    const static VkSamplerYcbcrConversionCreateInfo& defaultSamplerYcbcrConversionCreateInfo = {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        NULL,
        frameImageFormat,
        samplerYcbcrModelConversion,
        samplerYcbcrRange,
        { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY },
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_FILTER_NEAREST,
        false
    };

    // Create Vulkan's RenderPass
    pVideoRenderer->renderPass_.CreateRenderPass(&pVideoRenderer->device_, ctx.format.format);

    pVideoRenderer->render_.CreatePerDrawContexts(&pVideoRenderer->device_, ctx.swapchain, &ctx.extent, &viewport_, &scissor_,
        &ctx.format, pVideoRenderer->renderPass_.getRenderPass(), &defaultSamplerInfo,
        &defaultSamplerYcbcrConversionCreateInfo);

    return 0;
}

void VulkanFrame::detach_swapchain() { }

void VulkanFrame::prepare_viewport(const VkExtent2D& extent)
{
    extent_ = extent;

    viewport_.x = 0.0f;
    viewport_.y = 0.0f;
    viewport_.width = static_cast<float>(extent.width);
    viewport_.height = static_cast<float>(extent.height);
    viewport_.minDepth = 0.0f;
    viewport_.maxDepth = 1.0f;

    scissor_.offset = { 0, 0 };
    scissor_.extent = extent_;
}

void VulkanFrame::update_camera()
{
    const glm::vec3 center(0.0f);
    const glm::vec3 up(0.f, 0.0f, 1.0f);
    const glm::mat4 view = glm::lookAt(camera_.eye_pos, center, up);

    // Vulkan clip space has inverted Y and half Z.
    const glm::mat4 clip(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f);

    // camera_.view_projection = clip * projection * view;

    camera_.view_projection = clip * view;
}

void VulkanFrame::on_key(Key key)
{
    switch (key) {
    case KEY_SHUTDOWN:
    case KEY_ESC:
        quit();
        break;
    case KEY_UP:
    case KEY_PAGE_UP:
        camera_.eye_pos -= glm::vec3(0.05f);
        update_camera();
        break;
    case KEY_DOWN:
    case KEY_PAGE_DOWN:
        camera_.eye_pos += glm::vec3(0.05f);
        update_camera();
        break;
    case KEY_LEFT:
        camera_.eye_pos += glm::vec3(0.5f);
        update_camera();
        break;
    case KEY_RIGHT:
        camera_.eye_pos -= glm::vec3(0.5f);
        update_camera();
        break;
    case KEY_SPACE:
        codec_paused_ = !codec_paused_;
        break;
    default:
        break;
    }
}

void VulkanFrame::on_tick()
{
    if (codec_paused_)
        return;
}

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

void VulkanFrame::on_frame(bool trainFrame)
{
    const bool dumpDebug = false;
    frame_count++;

    FrameData& data = frame_data_[frame_data_index_];
    DecodedFrame* pLastDecodedFrame = NULL;

    if (m_videoProcessor && !trainFrame) {
        pLastDecodedFrame = &data.lastDecodedFrame;

        m_videoProcessor.ReleaseDisplayedFrame(pLastDecodedFrame);

        memset(pLastDecodedFrame, 0x00, sizeof(*pLastDecodedFrame));
        pLastDecodedFrame->pictureIndex = -1;

        bool endOfStream = false;
        int32_t numVideoFrames = 0;

        numVideoFrames = m_videoProcessor.GetNextFrames(pLastDecodedFrame, &endOfStream);
        if (endOfStream && (numVideoFrames < 0)) {
            quit();
        }
    }

    if (frame_count > 200) {
        // quit();
    }

    // Limit number of frames if argument was specified (with --c maxFrames)
    if (settings_.max_frame_count != -1 && frame_count == settings_.max_frame_count) {
        // Tell the FrameProcessor we're done after this frame is drawn.
        FrameProcessor::quit();
    }

    // wait for the last submission since we reuse frame data
    if (dumpDebug) {
        std::cout << "<= Wait on picIdx: " << pLastDecodedFrame->pictureIndex
                  << "\t\tdisplayOrder: " << pLastDecodedFrame->displayOrder
                  << "\tdecodeOrder: " << pLastDecodedFrame->decodeOrder
                  << "\ttimestamp " << pLastDecodedFrame->timestamp
                  << "\tdstImageView " << pLastDecodedFrame->pDecodedImage
                  << std::endl;
    }
    // vk::assert_success(vk::WaitForFences(pVideoRenderer->device_, 1, &data.lastDecodedFrame.frameConsumerDoneFence, true, 100 * 1000 * 1000 /* 100 mSec */));
    // vk::assert_success(vk::ResetFences(pVideoRenderer->device_, 1, &data.lastDecodedFrame.frameConsumerDoneFence));

    const Shell::BackBuffer& back = shell_->GetCurrentBackBuffer();
    assert(back.isInPrepareState());

    vulkanVideoUtils::VulkanPerDrawContext* pPerDrawContext = pVideoRenderer->render_.GetDrawContext(back.GetImageIndex());

    unsigned imageIndex = frame_data_index_;

    bool doTestPatternFrame = ((pLastDecodedFrame == NULL) || (pLastDecodedFrame->pDecodedImage == NULL) || pVideoRenderer->useTestImage_);
    const vulkanVideoUtils::ImageObject* pRtImage = NULL;
    VkFence frameCompleteFence = VkFence();
    VkFence frameConsumerDoneFence = VkFence();
    VkSemaphore frameCompleteSemaphore = VkSemaphore();
    VkSemaphore frameConsumerDoneSemaphore = VkSemaphore();
    VkQueryPool queryPool = VkQueryPool();
    int32_t startQueryId = -1;
    uint32_t numQueries = 0;
    int decodeOrder = 0;
    int displayOrder = 0;
    uint64_t timestamp = 0;
    if (doTestPatternFrame) {
        pRtImage = &pVideoRenderer->testFrameImage_;
    } else {
        pRtImage = pLastDecodedFrame->pDecodedImage;
        frameCompleteFence = pLastDecodedFrame->frameCompleteFence;
        frameCompleteSemaphore = pLastDecodedFrame->frameCompleteSemaphore;
        frameConsumerDoneSemaphore = pLastDecodedFrame->frameConsumerDoneSemaphore;
        frameConsumerDoneFence = pLastDecodedFrame->frameConsumerDoneFence;
        queryPool = pLastDecodedFrame->queryPool;
        startQueryId = pLastDecodedFrame->startQueryId;
        numQueries = pLastDecodedFrame->numQueries;
        imageIndex = pLastDecodedFrame->pictureIndex;
        decodeOrder = pLastDecodedFrame->decodeOrder;
        displayOrder = pLastDecodedFrame->displayOrder;
        timestamp = pLastDecodedFrame->timestamp;
    }

#ifdef NV_RMAPI_TEGRA
    if (pPerDrawContext->IsFormatOutOfDate(lastVideoFormatUpdate)) {
        const VkSamplerYcbcrConversionCreateInfo newSamplerYcbcrConversionCreateInfo = {
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            NULL,
            frameImageFormat,
            samplerYcbcrModelConversion,
            samplerYcbcrRange,
            { VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY },
            VK_CHROMA_LOCATION_MIDPOINT,
            VK_CHROMA_LOCATION_MIDPOINT,
            VK_FILTER_NEAREST,
            false
        };

        if (pPerDrawContext->samplerYcbcrConversion.SamplerRequiresUpdate(nullptr, &newSamplerYcbcrConversionCreateInfo)) {
            pVideoRenderer->render_.UpdatePerDrawContexts(pPerDrawContext, &viewport_, &scissor_,
                pVideoRenderer->renderPass_.getRenderPass(), &defaultSamplerInfo,
                &newSamplerYcbcrConversionCreateInfo);
        } else {
        }
    }
#endif // NV_RMAPI_TEGRA

    pPerDrawContext->bufferDescriptorSet.WriteDescriptorSet(VkSampler(0), pRtImage->view);

    pPerDrawContext->commandBuffer.CreateCommandBuffer(
        pVideoRenderer->renderPass_.getRenderPass(), pRtImage, pPerDrawContext->frameBuffer.GetFbImage(),
        pPerDrawContext->frameBuffer.GetFrameBuffer(), &scissor_, pPerDrawContext->gfxPipeline.getPipeline(),
        pPerDrawContext->bufferDescriptorSet.getPipelineLayout(), pPerDrawContext->bufferDescriptorSet.getDescriptorSet(),
        &pVideoRenderer->vertexBuffer_);

    if (dumpDebug) {
        LOG(INFO) << "Drawing Frame " << frame_count << " FB: " << back.GetImageIndex() << std::endl;
    }

    uint64_t curRealTimeNsecs = getNsTime();
    const float fpsDividend = 1000000000LL /* nSec in a Sec */;
    uint64_t deltaRealTimeNsecs = curRealTimeNsecs - lastRealTimeNsecs;
    lastRealTimeNsecs = curRealTimeNsecs;
    float fps = fpsDividend / deltaRealTimeNsecs;

    if (dumpDebug) {
        std::cout << "<= Present picIdx: " << imageIndex
                  << "\t\tdisplayOrder: " << displayOrder << "\tdecodeOrder: " << decodeOrder
                  << "\ttimestamp " << timestamp << "\tFPS: " << fps << "\tdstImageView "
                  << pRtImage->view << std::endl;
    }

    VkResult result = VK_SUCCESS;
    if (!pVideoRenderer->useTestImage_) {
        if (frameCompleteSemaphore == VkSemaphore()) {
            if (frameCompleteFence == VkFence()) {
                const Shell::Context& ctx = shell_->context();
                if (ctx.video_queue != VkQueue()) {
                    result = vk::QueueWaitIdle(ctx.video_queue);
                    assert(result == VK_SUCCESS);
                }
            } else {
                result = vk::WaitForFences(pVideoRenderer->device_, 1, &frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
                assert(result == VK_SUCCESS);
                result = vk::GetFenceStatus(pVideoRenderer->device_, frameCompleteFence);
                assert(result == VK_SUCCESS);
            }
        }
    }

    //For queryPool debugging
    bool getDecodeStatusBeforePresent = false;
    if (getDecodeStatusBeforePresent && (queryPool != VkQueryPool()) && (startQueryId >= 0) && (numQueries > 0)) {

        if (frameCompleteFence != VkFence()) {
            result = vk::WaitForFences(pVideoRenderer->device_, 1, &frameCompleteFence, true, 100 * 1000 * 1000 /* 100 mSec */);
            assert(result == VK_SUCCESS);
            result = vk::GetFenceStatus(pVideoRenderer->device_, frameCompleteFence);
            assert(result == VK_SUCCESS);
        }

        struct nvVideoGetDecodeStatus {
            VkQueryResultStatusKHR decodeStatus;
            uint32_t hwCyclesCount; /**< OUT: HW cycle count per frame         */
            uint32_t hwStatus; /**< OUT: HW decode status                 */
            uint32_t mbsCorrectlyDecoded; // total numers of correctly decoded macroblocks
            uint32_t mbsInError; // number of error macroblocks.
            uint16_t instanceId; /**< OUT: nvdec instance id                */
            uint16_t reserved1; /**< Reserved for future use               */
        } decodeStatus;
        result = vk::GetQueryPoolResults(pVideoRenderer->device_, queryPool, startQueryId, 1, sizeof(decodeStatus), &decodeStatus,
            512, VK_QUERY_RESULT_WAIT_BIT);
        assert(result == VK_SUCCESS);
        assert(decodeStatus.decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

        if (dumpDebug) {
            std::cout << "\t +++++++++++++++++++++++++++< " << imageIndex << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << imageIndex << std::endl
                      << "\t\tdecodeStatus: " << decodeStatus.decodeStatus << "\t\thwCyclesCount " << decodeStatus.hwCyclesCount
                      << "\t\thwStatus " << decodeStatus.hwStatus << "\t\tmbsCorrectlyDecoded " << decodeStatus.mbsCorrectlyDecoded
                      << "\t\tmbsInError " << decodeStatus.mbsInError
                      << "\t\tinstanceId " << decodeStatus.instanceId << std::endl;
        }
    }

    uint32_t waitSemaphoreCount = 0;
    VkSemaphore waitSemaphores[2] = {};

    if (back.GetAcquireSemaphore() != vkNullSemaphore) {
        waitSemaphores[waitSemaphoreCount++] = back.GetAcquireSemaphore();
    }

    if (frameCompleteSemaphore != VkSemaphore()) {
        waitSemaphores[waitSemaphoreCount++] = frameCompleteSemaphore;
    }

    uint32_t signalSemaphoreCount = 0;
    VkSemaphore signalSemaphores[2] = {};

    if (back.GetRenderSemaphore() != vkNullSemaphore) {
        signalSemaphores[signalSemaphoreCount++] = back.GetRenderSemaphore();
    }

    if (frameConsumerDoneSemaphore != VkSemaphore()) {
        signalSemaphores[signalSemaphoreCount++] = frameConsumerDoneSemaphore;
        pLastDecodedFrame->hasConsummerSignalSemaphore = true;
    }

    if (frameConsumerDoneFence != VkFence()) {
        pLastDecodedFrame->hasConsummerSignalFence = true;
    }

    // VkPipelineStageFlags primaryCmdSubmitWaitStages_ = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    // primary_cmd_submit_info_.pWaitDstStageMask = &primaryCmdSubmitWaitStages_;

    // wait for the image to be owned and signal for render completion

    VkPipelineStageFlags primary_cmd_submit_wait_stages[2] = { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    VkSubmitInfo primary_cmd_submit_info = VkSubmitInfo();

    primary_cmd_submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    primary_cmd_submit_info.pWaitDstStageMask = primary_cmd_submit_wait_stages;
    primary_cmd_submit_info.commandBufferCount = 1;

    primary_cmd_submit_info.waitSemaphoreCount = waitSemaphoreCount;
    primary_cmd_submit_info.pWaitSemaphores = waitSemaphoreCount ? waitSemaphores : NULL;
    primary_cmd_submit_info.pCommandBuffers = pPerDrawContext->commandBuffer.getCommandBuffer();

    primary_cmd_submit_info.signalSemaphoreCount = signalSemaphoreCount;
    primary_cmd_submit_info.pSignalSemaphores = signalSemaphoreCount ? signalSemaphores : NULL;

    // For fence/sync debugging
    if (false && frameCompleteFence) {
        result = vk::WaitForFences(pVideoRenderer->device_, 1, &frameCompleteFence, true, 100 * 1000 * 1000);
        assert(result == VK_SUCCESS);
        result = vk::GetFenceStatus(pVideoRenderer->device_, frameCompleteFence);
        assert(result == VK_SUCCESS);
    }

    VkResult res = vk::QueueSubmit(queue_, 1, &primary_cmd_submit_info, frameConsumerDoneFence);

    if (false && frameConsumerDoneFence) { // For fence/sync debugging
        const uint64_t fenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;
        result = vk::WaitForFences(pVideoRenderer->device_, 1, &frameConsumerDoneFence, true, fenceTimeout);
        assert(result == VK_SUCCESS);
        result = vk::GetFenceStatus(pVideoRenderer->device_, frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
    }

#if 0 // NV_RMAPI_TEGRA
    // TODO: Add Tegra fence_fd semaphore
    int fd = -1; // VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    const VkFenceGetFdInfoKHR getFdInfo =  { VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, NULL, data.lastDecodedFrame.frameConsumerDoneFence, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    res = vk::GetFenceFdKHR(pVideoRenderer->device_, &getFdInfo, &fd);
    close(fd);
#endif

    frame_data_index_ = (frame_data_index_ + 1) % frame_data_.size();

    (void)res;
}

#include "VulkanFrame.h"

FrameProcessor* create_frameProcessor(int argc, char** argv)
{
    std::vector<std::string> args(argv, argv + argc);
    return new VulkanFrame(args);
}
