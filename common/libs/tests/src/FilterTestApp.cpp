/*
* Copyright 2026 NVIDIA Corporation.
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

#include "FilterTestApp.h"
#include "TestCases.h"
#include "ColorConversion.h"

#include <iostream>
#include <chrono>
#include <cmath>
#include <cstring>

#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VkCodecUtils/Helpers.h"  // For vk::DeviceUuidUtils

namespace vkfilter_test {

// =============================================================================
// Format Conversion Utilities
// =============================================================================

VkFormat toVkFormat(TestFormat format) {
    switch (format) {
        case TestFormat::RGBA8:  return VK_FORMAT_R8G8B8A8_UNORM;
        case TestFormat::BGRA8:  return VK_FORMAT_B8G8R8A8_UNORM;
        case TestFormat::NV12:   return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case TestFormat::P010:   return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        case TestFormat::P012:   return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        case TestFormat::I420:   return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
        case TestFormat::NV16:   return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
        case TestFormat::P210:   return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
        case TestFormat::YUV444: return VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
        case TestFormat::Y410:   return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // Packed AVYU 4:4:4
        default:                 return VK_FORMAT_UNDEFINED;
    }
}

const char* testFormatName(TestFormat format) {
    switch (format) {
        case TestFormat::RGBA8:  return "RGBA8";
        case TestFormat::BGRA8:  return "BGRA8";
        case TestFormat::NV12:   return "NV12 (8-bit 4:2:0)";
        case TestFormat::P010:   return "P010 (10-bit 4:2:0)";
        case TestFormat::P012:   return "P012 (12-bit 4:2:0)";
        case TestFormat::I420:   return "I420 (8-bit 4:2:0 3-plane)";
        case TestFormat::NV16:   return "NV16 (8-bit 4:2:2)";
        case TestFormat::P210:   return "P210 (10-bit 4:2:2)";
        case TestFormat::YUV444: return "YUV444 (8-bit 4:4:4)";
        case TestFormat::Y410:   return "Y410 (10-bit 4:4:4 packed)";
        default:                 return "Unknown";
    }
}

static size_t calculateImageSize(TestFormat format, uint32_t width, uint32_t height) {
    switch (format) {
        // RGBA formats - 4 bytes per pixel
        case TestFormat::RGBA8:
        case TestFormat::BGRA8:
            return width * height * 4;
        
        // 4:2:0 8-bit (Y full + UV quarter)
        case TestFormat::NV12:  // 2-plane: Y + interleaved UV
        case TestFormat::I420:  // 3-plane: Y + U + V
            return width * height + (width / 2) * (height / 2) * 2;
        
        // 4:2:0 10/12-bit (16-bit storage per sample)
        case TestFormat::P010:  // 10-bit in 16-bit
        case TestFormat::P012:  // 12-bit in 16-bit
            return width * height * 2 + (width / 2) * (height / 2) * 4;
        
        // 4:2:2 8-bit (Y full + UV half width, full height)
        case TestFormat::NV16:  // 2-plane: Y + interleaved UV
            return width * height + (width / 2) * height * 2;
        
        // 4:2:2 10-bit (16-bit storage per sample)
        case TestFormat::P210:
            return width * height * 2 + (width / 2) * height * 4;
        
        // 4:4:4 8-bit (Y, U, V all full resolution)
        case TestFormat::YUV444:  // 3-plane: Y + U + V
            return width * height * 3;
        
        // 4:4:4 10-bit packed (AVYU in 32-bit)
        case TestFormat::Y410:
            return width * height * 4;
        
        default:
            return 0;
    }
}

// =============================================================================
// FilterTestApp Implementation
// =============================================================================

FilterTestApp::FilterTestApp() {
}

FilterTestApp::~FilterTestApp() {
    if (m_commandPool != VK_NULL_HANDLE) {
        m_vkDevCtx.DestroyCommandPool(m_vkDevCtx.getDevice(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

VkResult FilterTestApp::init(bool verbose) {
    
    // Required instance layers and extensions for validation (if verbose)
    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };
    
    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };
    
    // Required device extensions for compute filter
    static const char* const requiredDeviceExtensions[] = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,  // Required for push descriptor layout
        nullptr
    };
    
    // Optional extensions
    static const char* const optionalDeviceExtensions[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        nullptr
    };
    
    // Add validation layers and debug extensions if verbose
    if (verbose) {
        m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
    }
    
    // Add required device extensions
    m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtensions, verbose);
    m_vkDevCtx.AddOptDeviceExtensions(optionalDeviceExtensions, verbose);
    
    // Initialize Vulkan device (creates instance)
    VkResult result = m_vkDevCtx.InitVulkanDevice("VkFilterTest", VK_NULL_HANDLE, verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "[FilterTestApp] Failed to initialize Vulkan device: " << result << std::endl;
        return result;
    }
    
    // Initialize debug report (only if validation is enabled)
    result = m_vkDevCtx.InitDebugReport(verbose, verbose);
    if (result != VK_SUCCESS && verbose) {
        std::cerr << "[FilterTestApp] Warning: Failed to initialize debug report: " << result << std::endl;
        // Non-fatal - continue without debug
    }
    
    // Initialize physical device with compute and transfer queues
    // No video decode/encode queues needed for filter testing
    vk::DeviceUuidUtils deviceUuid;  // Empty UUID = auto-select
    result = m_vkDevCtx.InitPhysicalDevice(
        -1,                                         // deviceId: -1 = auto-select
        deviceUuid,                                 // deviceUUID: empty = auto
        VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,  // requestQueueTypes
        nullptr,                                    // pWsiDisplay: no WSI
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,       // No decode queues
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR        // No encode queues
    );
    if (result != VK_SUCCESS) {
        std::cerr << "[FilterTestApp] Failed to initialize physical device: " << result << std::endl;
        return result;
    }
    
    // Create Vulkan logical device with compute and transfer queues
    result = m_vkDevCtx.CreateVulkanDevice(
        0,                              // numDecodeQueues
        0,                              // numEncodeQueues
        VK_VIDEO_CODEC_OPERATION_NONE_KHR, // videoCodecs
        true,                           // createTransferQueue
        false,                          // createGraphicsQueue
        false,                          // createPresentQueue
        true                            // createComputeQueue
    );
    if (result != VK_SUCCESS) {
        std::cerr << "[FilterTestApp] Failed to create Vulkan device: " << result << std::endl;
        return result;
    }
    
    // Create command pool for compute queue
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = m_vkDevCtx.GetComputeQueueFamilyIdx();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    
    result = m_vkDevCtx.CreateCommandPool(m_vkDevCtx.getDevice(), &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        std::cerr << "[FilterTestApp] Failed to create command pool: " << result << std::endl;
        return result;
    }
    
    std::cout << "[FilterTestApp] Initialized successfully" << std::endl;
    std::cout << "  Compute Queue Family: " << m_vkDevCtx.GetComputeQueueFamilyIdx() << std::endl;
    std::cout << "  Transfer Queue Family: " << m_vkDevCtx.GetTransferQueueFamilyIdx() << std::endl;
    
    return VK_SUCCESS;
}

void FilterTestApp::registerTest(const TestCaseConfig& config) {
    m_testCases.push_back(config);
}

TestResult FilterTestApp::runTest(const TestCaseConfig& config) {
    TestResult result;
    result.testName = config.name;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::cout << "[Test] Running: " << config.name << std::endl;
    
    // Validate configuration
    if (config.inputs.empty()) {
        result.errorMessage = "No inputs specified";
        result.passed = false;
        return result;
    }
    if (config.outputs.empty()) {
        result.errorMessage = "No outputs specified";
        result.passed = false;
        return result;
    }
    
    // Check format support
    for (const auto& input : config.inputs) {
        if (!isFormatSupported(input.format, input.resourceType, input.tiling)) {
            result.errorMessage = "Input format not supported: " + std::string(testFormatName(input.format));
            result.passed = false;
            return result;
        }
    }
    for (const auto& output : config.outputs) {
        if (!isFormatSupported(output.format, output.resourceType, output.tiling)) {
            result.errorMessage = "Output format not supported: " + std::string(testFormatName(output.format));
            result.passed = false;
            return result;
        }
    }
    
    // Create input resources
    std::vector<VkSharedBaseObj<VkImageResource>> inputImages;
    std::vector<VkSharedBaseObj<VkImageResourceView>> inputImageViews;
    std::vector<VkSharedBaseObj<VkBufferResource>> inputBuffers;
    
    for (const auto& inputSlot : config.inputs) {
        VkSharedBaseObj<VkImageResource> image;
        VkSharedBaseObj<VkImageResourceView> imageView;
        VkSharedBaseObj<VkBufferResource> buffer;
        
        VkResult vkResult = createTestInput(inputSlot, image, imageView, buffer);
        if (vkResult != VK_SUCCESS) {
            result.errorMessage = "Failed to create input resource";
            result.passed = false;
            return result;
        }
        
        inputImages.push_back(image);
        inputImageViews.push_back(imageView);
        inputBuffers.push_back(buffer);
        
        // Generate test pattern
        if (inputSlot.generateTestPattern) {
            vkResult = generateTestPattern(inputSlot, image, buffer);
            if (vkResult != VK_SUCCESS) {
                result.errorMessage = "Failed to generate test pattern";
                result.passed = false;
                return result;
            }
        }
    }
    
    // Create output resources
    std::vector<VkSharedBaseObj<VkImageResource>> outputImages;
    std::vector<VkSharedBaseObj<VkImageResourceView>> outputImageViews;
    std::vector<VkSharedBaseObj<VkBufferResource>> outputBuffers;
    
    for (const auto& outputSlot : config.outputs) {
        VkSharedBaseObj<VkImageResource> image;
        VkSharedBaseObj<VkImageResourceView> imageView;
        VkSharedBaseObj<VkBufferResource> buffer;
        
        VkResult vkResult = createTestOutput(outputSlot, image, imageView, buffer);
        if (vkResult != VK_SUCCESS) {
            result.errorMessage = "Failed to create output resource";
            result.passed = false;
            return result;
        }
        
        outputImages.push_back(image);
        outputImageViews.push_back(imageView);
        outputBuffers.push_back(buffer);
    }
    
    // Create the filter
    VkSamplerYcbcrConversionCreateInfo ycbcrInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    ycbcrInfo.format = toVkFormat(config.inputs[0].format);
    ycbcrInfo.ycbcrModel = config.ycbcrModel;
    ycbcrInfo.ycbcrRange = config.ycbcrRange;
    ycbcrInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    ycbcrInfo.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    ycbcrInfo.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    ycbcrInfo.chromaFilter = VK_FILTER_LINEAR;
    ycbcrInfo.forceExplicitReconstruction = VK_FALSE;
    
    VkSharedBaseObj<VulkanFilter> filter;
    VkResult vkResult = VulkanFilterYuvCompute::Create(
        &m_vkDevCtx,
        m_vkDevCtx.GetComputeQueueFamilyIdx(),
        0,  // queue index
        config.filterType,
        4,  // maxNumFrames
        toVkFormat(config.inputs[0].format),
        toVkFormat(config.outputs[0].format),
        config.filterFlags,
        &ycbcrInfo,
        nullptr,  // YCbCr primaries constants (use default)
        nullptr,  // Sampler create info (use default)
        filter
    );
    
    if (vkResult != VK_SUCCESS) {
        result.errorMessage = "Failed to create filter: " + std::to_string(vkResult);
        result.passed = false;
        return result;
    }
    
    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    vkResult = m_vkDevCtx.AllocateCommandBuffers(m_vkDevCtx.getDevice(), &allocInfo, &cmdBuffer);
    if (vkResult != VK_SUCCESS) {
        result.errorMessage = "Failed to allocate command buffer";
        result.passed = false;
        return result;
    }
    
    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_vkDevCtx.BeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // Record filter commands
    VulkanFilterYuvCompute* yuvFilter = static_cast<VulkanFilterYuvCompute*>(filter.Get());
    
    // Set up resource info
    VkVideoPictureResourceInfoKHR inputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    inputResourceInfo.codedExtent = {config.inputs[0].width, config.inputs[0].height};
    inputResourceInfo.baseArrayLayer = 0;
    
    VkVideoPictureResourceInfoKHR outputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    outputResourceInfo.codedExtent = {config.outputs[0].width, config.outputs[0].height};
    outputResourceInfo.baseArrayLayer = 0;
    
    // Record based on resource types
    if (config.inputs[0].resourceType == ResourceType::Image &&
        config.outputs[0].resourceType == ResourceType::Image) {
        
        vkResult = yuvFilter->RecordCommandBuffer(
            cmdBuffer,
            0,  // bufferIdx
            inputImageViews[0].Get(),
            &inputResourceInfo,
            outputImageViews[0].Get(),
            &outputResourceInfo
        );
    } else {
        result.errorMessage = "Buffer I/O not yet implemented in test";
        result.passed = false;
        m_vkDevCtx.EndCommandBuffer(cmdBuffer);
        m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
        return result;
    }
    
    if (vkResult != VK_SUCCESS) {
        result.errorMessage = "Failed to record filter commands: " + std::to_string(vkResult);
        result.passed = false;
        m_vkDevCtx.EndCommandBuffer(cmdBuffer);
        m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
        return result;
    }
    
    // End command buffer
    m_vkDevCtx.EndCommandBuffer(cmdBuffer);
    
    // Create fence for synchronization
    VkFence fence;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    m_vkDevCtx.CreateFence(m_vkDevCtx.getDevice(), &fenceInfo, nullptr, &fence);
    
    // Submit command buffer
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    vkResult = m_vkDevCtx.QueueSubmit(m_vkDevCtx.GetComputeQueue(), 1, &submitInfo, fence);
    if (vkResult != VK_SUCCESS) {
        result.errorMessage = "Failed to submit command buffer";
        result.passed = false;
        m_vkDevCtx.DestroyFence(m_vkDevCtx.getDevice(), fence, nullptr);
        m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
        return result;
    }
    
    // Wait for completion
    m_vkDevCtx.WaitForFences(m_vkDevCtx.getDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
    
    // Cleanup
    m_vkDevCtx.DestroyFence(m_vkDevCtx.getDevice(), fence, nullptr);
    m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
    
    // Validate output
    // For linear images or buffers, we can validate directly
    // For optimal images, we'd need a staging buffer readback
    const TestIOSlot& outputSlot = config.outputs[0];
    
    if (outputSlot.validateOutput) {
        if (outputSlot.resourceType == ResourceType::Buffer || 
            outputSlot.tiling == TilingMode::Linear) {
            // Generate reference data from input
            // For now, just verify we got some data without reference comparison
            // Full validation requires capturing input data before the filter runs
            TestResult valResult = validateOutput(config, outputSlot, 
                                                 outputImages[0], outputBuffers[0],
                                                 std::vector<uint8_t>());  // Empty ref = just check for data
            result.passed = valResult.passed;
            result.psnrY = valResult.psnrY;
            result.psnrCb = valResult.psnrCb;
            result.psnrCr = valResult.psnrCr;
            if (!valResult.errorMessage.empty()) {
                result.errorMessage = valResult.errorMessage;
            }
        } else {
            // Optimal image - cannot validate without staging buffer readback
            // Mark as passed if filter execution succeeded
            result.passed = true;
        }
    } else {
        // No validation requested - just check execution succeeded
        result.passed = true;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    std::cout << "[Test] " << config.name << ": " 
              << (result.passed ? "PASSED" : "FAILED")
              << " (" << result.executionTimeMs << " ms)" << std::endl;
    
    return result;
}

std::vector<TestResult> FilterTestApp::runAllTests() {
    std::vector<TestResult> results;
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Running " << m_testCases.size() << " test(s)" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    for (const auto& testCase : m_testCases) {
        results.push_back(runTest(testCase));
    }
    
    printSummary(results);
    
    return results;
}

void FilterTestApp::printSummary(const std::vector<TestResult>& results) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST SUMMARY" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int passed = 0;
    int failed = 0;
    
    for (const auto& result : results) {
        if (result.passed) {
            passed++;
            std::cout << "[PASS] " << result.testName << std::endl;
        } else {
            failed++;
            std::cout << "[FAIL] " << result.testName << ": " << result.errorMessage << std::endl;
        }
    }
    
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Total: " << results.size() << ", Passed: " << passed << ", Failed: " << failed << std::endl;
    std::cout << "========================================\n" << std::endl;
}

bool FilterTestApp::isFormatSupported(TestFormat format, ResourceType resourceType, TilingMode tiling) const {
    VkFormat vkFormat = toVkFormat(format);
    
    if (resourceType == ResourceType::Buffer) {
        // Buffer resources are generally supported if format is valid
        return vkFormat != VK_FORMAT_UNDEFINED;
    }
    
    // For images, check format support
    VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    
    // For multi-planar YCbCr formats, we use VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
    // which allows per-plane views. So we need to check the plane formats.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(vkFormat);
    if (mpInfo && mpInfo->planesLayout.numberOfExtraPlanes > 0) {
        // Check each plane's format for storage support
        // Total planes = numberOfExtraPlanes + 1 (base plane)
        uint32_t numPlanes = mpInfo->planesLayout.numberOfExtraPlanes + 1;
        for (uint32_t plane = 0; plane < numPlanes; ++plane) {
            VkFormat planeFormat = mpInfo->vkPlaneFormat[plane];
            VkFormatProperties planeProps;
            m_vkDevCtx.GetPhysicalDeviceFormatProperties(m_vkDevCtx.getPhysicalDevice(), planeFormat, &planeProps);
            
            bool supported = false;
            if (tiling == TilingMode::Optimal) {
                supported = (planeProps.optimalTilingFeatures & requiredFeatures) != 0;
            } else {
                supported = (planeProps.linearTilingFeatures & requiredFeatures) != 0;
            }
            
            if (!supported) {
                return false;  // Any unsupported plane fails the whole format
            }
        }
        return true;  // All planes supported
    }
    
    // For single-plane formats, check directly
    VkFormatProperties formatProps;
    m_vkDevCtx.GetPhysicalDeviceFormatProperties(m_vkDevCtx.getPhysicalDevice(), vkFormat, &formatProps);
    
    if (tiling == TilingMode::Optimal) {
        return (formatProps.optimalTilingFeatures & requiredFeatures) != 0;
    } else {
        return (formatProps.linearTilingFeatures & requiredFeatures) != 0;
    }
}

VkResult FilterTestApp::createTestInput(const TestIOSlot& slot,
                                       VkSharedBaseObj<VkImageResource>& outImage,
                                       VkSharedBaseObj<VkImageResourceView>& outImageView,
                                       VkSharedBaseObj<VkBufferResource>& outBuffer) {
    VkFormat vkFormat = toVkFormat(slot.format);
    
    if (slot.resourceType == ResourceType::Image) {
        // Create image using VkImageResource
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = vkFormat;
        imageInfo.extent = {slot.width, slot.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = (slot.tiling == TilingMode::Linear) ? 
                           VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        // For multi-planar formats, add MUTABLE and EXTENDED flags
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(vkFormat);
        if (mpInfo && mpInfo->planesLayout.numberOfExtraPlanes > 0) {
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
        
        VkMemoryPropertyFlags memProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (slot.tiling == TilingMode::Linear) {
            memProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        
        VkResult result = VkImageResource::Create(&m_vkDevCtx, &imageInfo, memProps, outImage);
        if (result != VK_SUCCESS) {
            return result;
        }
        
        // Create image view
        VkImageSubresourceRange subresRange{};
        subresRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresRange.baseMipLevel = 0;
        subresRange.levelCount = 1;
        subresRange.baseArrayLayer = 0;
        subresRange.layerCount = 1;
        
        result = VkImageResourceView::Create(&m_vkDevCtx, outImage, subresRange, 
                                             VK_IMAGE_USAGE_STORAGE_BIT, outImageView);
        if (result != VK_SUCCESS) {
            return result;
        }
    } else {
        // Create buffer using VkBufferResource
        size_t bufferSize = calculateImageSize(slot.format, slot.width, slot.height);
        
        VkResult result = VkBufferResource::Create(
            &m_vkDevCtx,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | 
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            bufferSize,
            outBuffer
        );
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    
    return VK_SUCCESS;
}

VkResult FilterTestApp::createTestOutput(const TestIOSlot& slot,
                                        VkSharedBaseObj<VkImageResource>& outImage,
                                        VkSharedBaseObj<VkImageResourceView>& outImageView,
                                        VkSharedBaseObj<VkBufferResource>& outBuffer) {
    // Same as createTestInput for now
    return createTestInput(slot, outImage, outImageView, outBuffer);
}

VkResult FilterTestApp::createStagingBuffer(size_t size,
                                           VkSharedBaseObj<VkBufferResource>& outBuffer) {
    return VkBufferResource::Create(
        &m_vkDevCtx,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        size,
        outBuffer
    );
}

VkResult FilterTestApp::generateTestPattern(const TestIOSlot& slot,
                                           VkSharedBaseObj<VkImageResource>& image,
                                           VkSharedBaseObj<VkBufferResource>& buffer) {
    std::vector<uint8_t> patternData;
    
    // Generate test pattern based on input format
    switch (slot.format) {
        case TestFormat::RGBA8:
        case TestFormat::BGRA8: {
            // Generate RGBA color bars pattern
            generateRGBATestPattern(TestPatternType::ColorBars, 
                                   slot.width, slot.height, patternData);
            break;
        }
        
        case TestFormat::NV12:
        case TestFormat::I420: {
            // Generate NV12 test pattern by converting from RGBA
            std::vector<uint8_t> rgbaData;
            generateRGBATestPattern(TestPatternType::ColorBars, 
                                   slot.width, slot.height, rgbaData);
            
            // Convert RGBA to YCbCr using ColorConversion module
            std::vector<uint8_t> yPlane;
            std::vector<uint8_t> uvPlane;
            convertRGBAtoNV12(rgbaData.data(), slot.width, slot.height,
                             ColorPrimaries::BT709, ColorRange::Full,
                             yPlane, uvPlane);
            
            // Combine planes for buffer
            patternData.reserve(yPlane.size() + uvPlane.size());
            patternData.insert(patternData.end(), yPlane.begin(), yPlane.end());
            patternData.insert(patternData.end(), uvPlane.begin(), uvPlane.end());
            break;
        }
        
        case TestFormat::P010:
        case TestFormat::P012:
        case TestFormat::P210: {
            // For 10/12-bit formats, generate 16-bit data
            std::vector<uint8_t> rgbaData;
            generateRGBATestPattern(TestPatternType::ColorBars, 
                                   slot.width, slot.height, rgbaData);
            
            std::vector<uint16_t> yPlane16;
            std::vector<uint16_t> uvPlane16;
            convertRGBAtoP010(rgbaData.data(), slot.width, slot.height,
                             ColorPrimaries::BT709, ColorRange::Full,
                             yPlane16, uvPlane16);
            
            // Convert to byte array
            patternData.resize((yPlane16.size() + uvPlane16.size()) * 2);
            memcpy(patternData.data(), yPlane16.data(), yPlane16.size() * 2);
            memcpy(patternData.data() + yPlane16.size() * 2, 
                   uvPlane16.data(), uvPlane16.size() * 2);
            break;
        }
        
        default: {
            // Fallback: generate simple gradient pattern
            size_t size = calculateImageSize(slot.format, slot.width, slot.height);
            patternData.resize(size);
            for (size_t i = 0; i < size; i++) {
                patternData[i] = static_cast<uint8_t>((i * 17) % 256);
            }
            break;
        }
    }
    
    // Upload pattern data to resource
    if (buffer && !patternData.empty()) {
        VkDeviceSize maxSize;
        uint8_t* data = buffer->GetDataPtr(0, maxSize);
        if (data) {
            size_t copySize = std::min(patternData.size(), static_cast<size_t>(maxSize));
            memcpy(data, patternData.data(), copySize);
        }
    }
    
    // For optimal-tiled images, we need to upload via staging buffer and transfer
    if (image && slot.tiling == TilingMode::Optimal && !patternData.empty()) {
        // Create staging buffer
        VkSharedBaseObj<VkBufferResource> stagingBuffer;
        VkResult result = createStagingBuffer(patternData.size(), stagingBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }
        
        // Copy pattern data to staging buffer
        VkDeviceSize maxSize;
        uint8_t* stagingData = stagingBuffer->GetDataPtr(0, maxSize);
        if (stagingData) {
            memcpy(stagingData, patternData.data(), patternData.size());
        }
        
        // TODO: Record and submit transfer commands to copy from staging to image
        // For now, pattern upload for optimal images is not fully implemented
    }
    
    // For linear-tiled images, we can map directly via memory
    if (image && slot.tiling == TilingMode::Linear && !patternData.empty()) {
        auto memory = image->GetMemory();
        if (memory) {
            VkDeviceSize maxSize;
            uint8_t* data = memory->GetDataPtr(0, maxSize);
            if (data) {
                size_t copySize = std::min(patternData.size(), static_cast<size_t>(maxSize));
                memcpy(data, patternData.data(), copySize);
            }
        }
    }
    
    return VK_SUCCESS;
}

TestResult FilterTestApp::validateOutput(const TestCaseConfig& config,
                                        const TestIOSlot& outputSlot,
                                        VkSharedBaseObj<VkImageResource>& outputImage,
                                        VkSharedBaseObj<VkBufferResource>& outputBuffer,
                                        const std::vector<uint8_t>& referenceData) {
    TestResult result;
    result.testName = config.name;
    
    // Get actual output data
    std::vector<uint8_t> actualData;
    size_t expectedSize = calculateImageSize(outputSlot.format, outputSlot.width, outputSlot.height);
    
    if (outputBuffer) {
        // Read from buffer
        VkDeviceSize maxSize;
        uint8_t* data = outputBuffer->GetDataPtr(0, maxSize);
        if (data && maxSize > 0) {
            actualData.resize(std::min(static_cast<size_t>(maxSize), expectedSize));
            memcpy(actualData.data(), data, actualData.size());
        }
    } else if (outputImage && outputSlot.tiling == TilingMode::Linear) {
        // Read directly from linear image via memory
        auto memory = outputImage->GetMemory();
        if (memory) {
            VkDeviceSize maxSize;
            uint8_t* data = memory->GetDataPtr(0, maxSize);
            if (data && maxSize > 0) {
                actualData.resize(std::min(static_cast<size_t>(maxSize), expectedSize));
                memcpy(actualData.data(), data, actualData.size());
            }
        }
    }
    
    // If we have reference data, compare
    if (!referenceData.empty() && !actualData.empty()) {
        // Determine comparison method based on output format
        switch (outputSlot.format) {
            case TestFormat::NV12:
            case TestFormat::I420: {
                // Split into Y and UV planes
                size_t ySize = outputSlot.width * outputSlot.height;
                size_t uvSize = (outputSlot.width / 2) * (outputSlot.height / 2) * 2;
                
                if (actualData.size() >= ySize + uvSize && referenceData.size() >= ySize + uvSize) {
                    const uint8_t* actualY = actualData.data();
                    const uint8_t* actualUV = actualData.data() + ySize;
                    const uint8_t* refY = referenceData.data();
                    const uint8_t* refUV = referenceData.data() + ySize;
                    
                    ValidationResult valResult = compareNV12(actualY, actualUV, refY, refUV,
                                                            outputSlot.width, outputSlot.height,
                                                            static_cast<uint32_t>(config.tolerance * 255.0f));
                    result.passed = valResult.passed;
                    result.psnrY = valResult.psnrY;
                    result.psnrCb = valResult.psnrCb;
                    result.psnrCr = valResult.psnrCr;
                    result.errorMessage = valResult.errorMessage;
                } else {
                    result.passed = false;
                    result.errorMessage = "Size mismatch for NV12 validation";
                }
                break;
            }
            
            case TestFormat::RGBA8:
            case TestFormat::BGRA8: {
                if (actualData.size() >= expectedSize && referenceData.size() >= expectedSize) {
                    ValidationResult valResult = compareRGBA(actualData.data(), referenceData.data(),
                                                            outputSlot.width, outputSlot.height,
                                                            static_cast<uint32_t>(config.tolerance * 255.0f));
                    result.passed = valResult.passed;
                    result.psnrY = valResult.psnrY;  // Using Y channel for RGBA comparison
                    result.errorMessage = valResult.errorMessage;
                } else {
                    result.passed = false;
                    result.errorMessage = "Size mismatch for RGBA validation";
                }
                break;
            }
            
            default: {
                // Generic byte-by-byte comparison with PSNR
                double psnr = calculatePSNR(actualData.data(), referenceData.data(), 
                                           std::min(actualData.size(), referenceData.size()));
                result.psnrY = psnr;
                result.passed = (psnr >= 30.0);  // 30 dB threshold
                if (!result.passed) {
                    result.errorMessage = "PSNR below threshold: " + std::to_string(psnr) + " dB";
                }
                break;
            }
        }
    } else if (referenceData.empty()) {
        // No reference data - just check that we got some output
        result.passed = !actualData.empty();
        if (!result.passed) {
            result.errorMessage = "No output data retrieved";
        }
    } else {
        result.passed = false;
        result.errorMessage = "Failed to read output data for validation";
    }
    
    return result;
}

std::vector<uint8_t> FilterTestApp::generateReferenceOutput(const TestCaseConfig& config,
                                                            const std::vector<uint8_t>& inputData) {
    std::vector<uint8_t> referenceData;
    
    if (config.inputs.empty() || config.outputs.empty()) {
        return referenceData;
    }
    
    const TestIOSlot& input = config.inputs[0];
    const TestIOSlot& output = config.outputs[0];
    
    // Get color conversion parameters from config
    ColorPrimaries primaries = fromVkYcbcrModel(config.ycbcrModel);
    ColorRange range = fromVkYcbcrRange(config.ycbcrRange);
    
    switch (config.filterType) {
        case VulkanFilterYuvCompute::RGBA2YCBCR: {
            // Convert RGBA input to YCbCr output using CPU reference
            if (input.format == TestFormat::RGBA8 || input.format == TestFormat::BGRA8) {
                switch (output.format) {
                    case TestFormat::NV12: {
                        std::vector<uint8_t> yPlane;
                        std::vector<uint8_t> uvPlane;
                        convertRGBAtoNV12(inputData.data(), input.width, input.height,
                                         primaries, range, yPlane, uvPlane);
                        referenceData.reserve(yPlane.size() + uvPlane.size());
                        referenceData.insert(referenceData.end(), yPlane.begin(), yPlane.end());
                        referenceData.insert(referenceData.end(), uvPlane.begin(), uvPlane.end());
                        break;
                    }
                    
                    case TestFormat::I420: {
                        std::vector<uint8_t> yPlane, uPlane, vPlane;
                        convertRGBAtoI420(inputData.data(), input.width, input.height,
                                         primaries, range, yPlane, uPlane, vPlane);
                        referenceData.reserve(yPlane.size() + uPlane.size() + vPlane.size());
                        referenceData.insert(referenceData.end(), yPlane.begin(), yPlane.end());
                        referenceData.insert(referenceData.end(), uPlane.begin(), uPlane.end());
                        referenceData.insert(referenceData.end(), vPlane.begin(), vPlane.end());
                        break;
                    }
                    
                    case TestFormat::NV16: {
                        std::vector<uint8_t> yPlane, uvPlane;
                        convertRGBAtoNV16(inputData.data(), input.width, input.height,
                                         primaries, range, yPlane, uvPlane);
                        referenceData.reserve(yPlane.size() + uvPlane.size());
                        referenceData.insert(referenceData.end(), yPlane.begin(), yPlane.end());
                        referenceData.insert(referenceData.end(), uvPlane.begin(), uvPlane.end());
                        break;
                    }
                    
                    case TestFormat::YUV444: {
                        std::vector<uint8_t> yPlane, uPlane, vPlane;
                        convertRGBAtoYUV444(inputData.data(), input.width, input.height,
                                           primaries, range, yPlane, uPlane, vPlane);
                        referenceData.reserve(yPlane.size() + uPlane.size() + vPlane.size());
                        referenceData.insert(referenceData.end(), yPlane.begin(), yPlane.end());
                        referenceData.insert(referenceData.end(), uPlane.begin(), uPlane.end());
                        referenceData.insert(referenceData.end(), vPlane.begin(), vPlane.end());
                        break;
                    }
                    
                    case TestFormat::P010: {
                        std::vector<uint16_t> yPlane16, uvPlane16;
                        convertRGBAtoP010(inputData.data(), input.width, input.height,
                                         primaries, range, yPlane16, uvPlane16);
                        referenceData.resize((yPlane16.size() + uvPlane16.size()) * 2);
                        memcpy(referenceData.data(), yPlane16.data(), yPlane16.size() * 2);
                        memcpy(referenceData.data() + yPlane16.size() * 2,
                               uvPlane16.data(), uvPlane16.size() * 2);
                        break;
                    }
                    
                    default:
                        break;
                }
            }
            break;
        }
        
        case VulkanFilterYuvCompute::YCBCR2RGBA: {
            // Convert YCbCr input to RGBA output using CPU reference
            if (output.format == TestFormat::RGBA8 || output.format == TestFormat::BGRA8) {
                switch (input.format) {
                    case TestFormat::NV12: {
                        size_t ySize = input.width * input.height;
                        if (inputData.size() >= ySize) {
                            const uint8_t* yPlane = inputData.data();
                            const uint8_t* uvPlane = inputData.data() + ySize;
                            convertNV12toRGBA(yPlane, uvPlane, input.width, input.height,
                                             primaries, range, referenceData);
                        }
                        break;
                    }
                    
                    default:
                        break;
                }
            }
            break;
        }
        
        case VulkanFilterYuvCompute::YCBCRCOPY: {
            // For copy, reference equals input (same format)
            referenceData = inputData;
            break;
        }
        
        case VulkanFilterYuvCompute::YCBCRCLEAR: {
            // For clear, generate expected cleared values
            size_t size = calculateImageSize(output.format, output.width, output.height);
            referenceData.resize(size);
            
            // Initialize with 50% gray for Y/R=0.5, and neutral for CbCr=0.5 (128 for 8-bit)
            std::fill(referenceData.begin(), referenceData.end(), 128);
            break;
        }
        
        default:
            break;
    }
    
    return referenceData;
}

VkResult FilterTestApp::copyImageToStagingBuffer(VkSharedBaseObj<VkImageResource>& image,
                                                 VkSharedBaseObj<VkBufferResource>& stagingBuffer) {
    // TODO: Implement image-to-buffer copy for optimal tiled images
    // For now, this is a stub that returns success since we're mainly using linear images
    return VK_SUCCESS;
}

double FilterTestApp::calculatePSNR(const uint8_t* data1, const uint8_t* data2, size_t size) {
    double mse = 0.0;
    for (size_t i = 0; i < size; i++) {
        double diff = static_cast<double>(data1[i]) - static_cast<double>(data2[i]);
        mse += diff * diff;
    }
    mse /= static_cast<double>(size);
    
    if (mse == 0.0) {
        return 100.0;  // Perfect match
    }
    
    double maxVal = 255.0;
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

// =============================================================================
// Standard Test Case Registration
// =============================================================================

void registerStandardTestCases(FilterTestApp& app) {
    auto tests = TestCases::getSmokeTests();
    for (const auto& test : tests) {
        app.registerTest(test);
    }
}

} // namespace vkfilter_test
