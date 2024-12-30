/*
 * Copyright 2022 NVIDIA Corporation.
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

#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkCodecUtils/VulkanVideoDisplayQueue.h"
#include "VkCodecUtils/VulkanVideoEncodeDisplayQueue.h"
#include "VkCodecUtils/VulkanEncoderFrameProcessor.h"
#include "VkShell/Shell.h"

int main(int argc, const char* argv[])
{
    VkSharedBaseObj<EncoderConfig> encoderConfig;
    if (VK_SUCCESS != EncoderConfig::CreateCodecConfig(argc, argv, encoderConfig)) {
        return -1;
    }

    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };

    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredWsiInstanceExtensions[] = {
        // Required generic WSI extensions
        VK_KHR_SURFACE_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredWsiDeviceExtension[] = {
        // Add the WSI required device extensions
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
        // VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
#endif
        nullptr
    };

    static const char* const optinalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        nullptr
    };

    VulkanDeviceContext vkDevCtxt;

    if (encoderConfig->validate) {
        vkDevCtxt.AddReqInstanceLayers(requiredInstanceLayers);
        vkDevCtxt.AddReqInstanceExtensions(requiredInstanceExtensions);
    }

    // Add the Vulkan video required device extensions
    vkDevCtxt.AddReqDeviceExtensions(requiredDeviceExtension);
    vkDevCtxt.AddOptDeviceExtensions(optinalDeviceExtension);

    /********** Start WSI instance extensions support *******************************************/
    if (encoderConfig->enableFramePresent) {
        const std::vector<VkExtensionProperties>& wsiRequiredInstanceInstanceExtensions =
                Shell::GetRequiredInstanceExtensions(encoderConfig->enableFrameDirectModePresent);

        for (size_t e = 0; e < wsiRequiredInstanceInstanceExtensions.size(); e++) {
            vkDevCtxt.AddReqInstanceExtension(wsiRequiredInstanceInstanceExtensions[e].extensionName);
        }

        // Add the WSI required instance extensions
        vkDevCtxt.AddReqInstanceExtensions(requiredWsiInstanceExtensions);

        // Add the WSI required device extensions
        vkDevCtxt.AddReqDeviceExtensions(requiredWsiDeviceExtension);
    }
    /********** End WSI instance extensions support *******************************************/

    VkResult result = vkDevCtxt.InitVulkanDevice(encoderConfig->appName.c_str(), VK_NULL_HANDLE,
                                                 encoderConfig->verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return -1;
    }

    result = vkDevCtxt.InitDebugReport(encoderConfig->validate,
                                       encoderConfig->validateVerbose);
    if (result != VK_SUCCESS) {
        return -1;
    }

    const bool supportsDisplay = true;
    const int32_t numEncodeQueues = ((encoderConfig->queueId != 0) ||
                                     (encoderConfig->enableHwLoadBalancing != 0)) ?
                                     -1 : // all available HW encoders
                                      1;  // only one HW encoder instance

    VkQueueFlags requestVideoEncodeQueueMask = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;


    if (encoderConfig->selectVideoWithComputeQueue) {
        requestVideoEncodeQueueMask |= VK_QUEUE_COMPUTE_BIT;
    }

    VkQueueFlags requestVideoComputeQueueMask = 0;
    if (encoderConfig->enablePreprocessComputeFilter == VK_TRUE) {
        requestVideoComputeQueueMask = VK_QUEUE_COMPUTE_BIT;
    }

    VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>> videoDispayQueue;
    result = CreateVulkanVideoEncodeDisplayQueue(&vkDevCtxt,
                                                 encoderConfig->encodeWidth,
                                                 encoderConfig->encodeHeight,
                                                 encoderConfig->input.bpp,
                                                 encoderConfig->input.vkFormat,
                                                 videoDispayQueue);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkSharedBaseObj<FrameProcessor> frameProcessor;
    result = CreateEncoderFrameProcessor(&vkDevCtxt, frameProcessor);
    if (result != VK_SUCCESS) {
        return -1;
    }

    VkSharedBaseObj<VkVideoEncoder> encoder; // the encoder's instance
    if (supportsDisplay && encoderConfig->enableFramePresent) {

        VkSharedBaseObj<Shell> displayShell;
        const Shell::Configuration configuration(encoderConfig->appName.c_str(),
                                                 4, // the display queue size
                                                 encoderConfig->enableFrameDirectModePresent);
        result = Shell::Create(&vkDevCtxt, configuration, displayShell);
        if (result != VK_SUCCESS) {
            assert(!"Can't allocate display shell! Out of memory!");
            return -1;
        }

        result = vkDevCtxt.InitPhysicalDevice(encoderConfig->deviceId, encoderConfig->deviceUUID,
                                              (VK_QUEUE_GRAPHICS_BIT |
                                              requestVideoComputeQueueMask |
                                              requestVideoEncodeQueueMask),
                                              displayShell,
                                              0,
                                              VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                              requestVideoEncodeQueueMask,
                                              (VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
                                               VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR));
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }
        assert(displayShell->PhysDeviceCanPresent(vkDevCtxt.getPhysicalDevice(),
                                                   vkDevCtxt.GetPresentQueueFamilyIdx()));

        result = vkDevCtxt.CreateVulkanDevice(0, // num decode queues
                                              numEncodeQueues,   // num encode queues
                                              encoderConfig->codec,
                                              false,             // createTransferQueue
                                              true,              // createGraphicsQueue
                                              true,              // createDisplayQueue
                                              ((encoderConfig->selectVideoWithComputeQueue == 1) ||  // createComputeQueue
                                               (encoderConfig->enablePreprocessComputeFilter == VK_TRUE))
                                              );
        if (result != VK_SUCCESS) {

            assert(!"Failed to create Vulkan device!");
            return -1;
        }

        result = VkVideoEncoder::CreateVideoEncoder(&vkDevCtxt, encoderConfig, encoder);
        if (result != VK_SUCCESS) {
            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }

        if (displayShell && videoDispayQueue) {
            VkSharedBaseObj<VkVideoRefCountBase> videoQueue(videoDispayQueue);
            frameProcessor->AttachQueue(videoQueue);
            displayShell->AttachFrameProcessor(frameProcessor);
            result = encoder->AttachDisplayQueue(displayShell, videoDispayQueue);
        }

        // std::this_thread::sleep_for(std::chrono::seconds(5));

    } else {

        // No display presentation and no decoder - just the encoder
        result = vkDevCtxt.InitPhysicalDevice(encoderConfig->deviceId, encoderConfig->deviceUUID,
                                              (requestVideoComputeQueueMask |
                                               requestVideoEncodeQueueMask  |
                                               VK_QUEUE_TRANSFER_BIT),
                                              nullptr,
                                              0,
                                              VK_VIDEO_CODEC_OPERATION_NONE_KHR,
                                              requestVideoEncodeQueueMask,
                                              encoderConfig->codec);
        if (result != VK_SUCCESS) {

            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }

        result = vkDevCtxt.CreateVulkanDevice(0, // num decode queues
                                              numEncodeQueues,     // num encode queues
                                              encoderConfig->codec,
                                              // If no graphics or compute queue is requested, only video queues
                                              // will be created. Not all implementations support transfer on video queues,
                                              // so request a separate transfer queue for such implementations.
                                              ((vkDevCtxt.GetVideoEncodeQueueFlag() & VK_QUEUE_TRANSFER_BIT) == 0), //  createTransferQueue
                                              false, // createGraphicsQueue
                                              false, // createDisplayQueue
                                              ((encoderConfig->selectVideoWithComputeQueue == 1) ||  // createComputeQueue
                                               (encoderConfig->enablePreprocessComputeFilter == VK_TRUE))
                                              );
        if (result != VK_SUCCESS) {

            assert(!"Failed to create Vulkan device!");
            return -1;
        }

        result = VkVideoEncoder::CreateVideoEncoder(&vkDevCtxt, encoderConfig, encoder);
        if (result != VK_SUCCESS) {
            assert(!"Can't initialize the Vulkan physical device!");
            return -1;
        }
    }

    // Enter the encoding frame loop
    uint32_t curFrameIndex = 0;
    for(; curFrameIndex < encoderConfig->numFrames; curFrameIndex++) {

        if (encoderConfig->verboseFrameStruct) {
            std::cout << "####################################################################################" << std::endl
                      << "Start processing current input frame index: " << curFrameIndex << std::endl;
        }

        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> encodeFrameInfo;
        encoder->GetAvailablePoolNode(encodeFrameInfo);
        assert(encodeFrameInfo);
        // load frame data from the file
        result = encoder->LoadNextFrame(encodeFrameInfo);
        if (result != VK_SUCCESS) {
            std::cout << "ERROR processing input frame index: " << curFrameIndex << std::endl;
            break;
        }

        if (encoderConfig->verboseFrameStruct) {
            std::cout << "End processing current input frame index: " << curFrameIndex << std::endl;
        }
    }

    encoder->WaitForThreadsToComplete();

    std::cout << "Done processing " << curFrameIndex << " input frames!" << std::endl
              << "Encoded file's location is at " << encoderConfig->outputFileHandler.GetFileName()
              << std::endl;
    return 0;
}
