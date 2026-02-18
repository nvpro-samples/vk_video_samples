/*
 * Copyright 2024-2026 NVIDIA Corporation.
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

#include "DrmFormatModTest.h"
#include "DrmFormats.h"

#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>
#include <vk_video/vulkan_video_codec_h264std_encode.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstring>
#include <unistd.h>

namespace drm_format_mod_test {

//=============================================================================
// VkFormat to String
//=============================================================================

const char* vkFormatToString(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return "VK_FORMAT_R8_UNORM";
        case VK_FORMAT_R16_UNORM: return "VK_FORMAT_R16_UNORM";
        case VK_FORMAT_R8G8_UNORM: return "VK_FORMAT_R8G8_UNORM";
        case VK_FORMAT_R16G16_UNORM: return "VK_FORMAT_R16G16_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_UNORM: return "VK_FORMAT_R16G16B16A16_UNORM";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "VK_FORMAT_R16G16B16A16_SFLOAT";
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: return "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM: return "VK_FORMAT_G16_B16R16_2PLANE_420_UNORM";
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM: return "VK_FORMAT_G8_B8R8_2PLANE_422_UNORM";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16: return "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16";
        case VK_FORMAT_G8B8G8R8_422_UNORM: return "VK_FORMAT_G8B8G8R8_422_UNORM";
        case VK_FORMAT_B8G8R8G8_422_UNORM: return "VK_FORMAT_B8G8R8G8_422_UNORM";
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16: return "VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16";
        default: return "UNKNOWN_FORMAT";
    }
}

//=============================================================================
// Test Status to String
//=============================================================================

const char* testStatusToString(TestStatus status) {
    switch (status) {
        case TestStatus::PASS: return "PASS";
        case TestStatus::FAIL: return "FAIL";
        case TestStatus::SKIP: return "SKIP";
        case TestStatus::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

//=============================================================================
// DrmFormatModTest Implementation
//=============================================================================

DrmFormatModTest::DrmFormatModTest() {
}

DrmFormatModTest::~DrmFormatModTest() {
    VkDevice device = m_vkDevCtx.getDevice();
    if (device != VK_NULL_HANDLE) {
        for (auto& h : m_importedHandles) {
            if (h.image != VK_NULL_HANDLE) m_vkDevCtx.DestroyImage(device, h.image, nullptr);
            if (h.memory != VK_NULL_HANDLE) m_vkDevCtx.FreeMemory(device, h.memory, nullptr);
        }
        m_importedHandles.clear();
        if (m_commandPool != VK_NULL_HANDLE) {
            m_vkDevCtx.DestroyCommandPool(device, m_commandPool, nullptr);
        }
    }
}

VkResult DrmFormatModTest::init(const TestConfig& config) {
    m_config = config;
    
    // Required instance layers/extensions for validation
    static const char* const requiredInstanceLayers[] = {
        "VK_LAYER_KHRONOS_validation",
        nullptr
    };
    
    static const char* const requiredInstanceExtensions[] = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        nullptr
    };
    
    // Required device extensions for DRM format modifiers
    static const char* const requiredDeviceExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        nullptr
    };
    
    // Video common extensions (shared by encode and decode)
    static const char* const videoCommonExtensions[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        nullptr
    };
    
    // Video encode extensions
    static const char* const videoEncodeExtensions[] = {
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        nullptr
    };
    
    // Video decode extensions
    static const char* const videoDecodeExtensions[] = {
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        nullptr
    };
    
    // Enable validation layers if requested (via --validation or --verbose implies validation)
    bool enableValidation = config.validation || config.verbose;
    
    // Add instance layers/extensions for validation
    if (enableValidation) {
        m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
        std::cout << "[INFO] Validation layers enabled" << std::endl;
    }
    
    // Add device extensions
    m_vkDevCtx.AddReqDeviceExtensions(requiredDeviceExtensions, config.verbose);
    
    // Add video extensions when testing video usage.
    // Use AddOptDeviceExtensions so HasAllDeviceExtensions doesn't reject the GPU
    // if it reports extensions differently when video queues are involved.
    if (config.videoEncode || config.videoDecode) {
        m_vkDevCtx.AddOptDeviceExtensions(videoCommonExtensions, config.verbose);
        if (config.videoEncode) {
            m_vkDevCtx.AddOptDeviceExtensions(videoEncodeExtensions, config.verbose);
            std::cout << "[INFO] Video encode extensions requested (--video-encode)" << std::endl;
        }
        if (config.videoDecode) {
            m_vkDevCtx.AddOptDeviceExtensions(videoDecodeExtensions, config.verbose);
            std::cout << "[INFO] Video decode extensions requested (--video-decode)" << std::endl;
        }
    }
    
    // Initialize Vulkan instance
    VkResult result = m_vkDevCtx.InitVulkanDevice("DrmFormatModTest", VK_NULL_HANDLE, config.verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to initialize Vulkan instance: " << result << std::endl;
        return result;
    }
    
    // Setup debug callback if validation is enabled
    if (enableValidation) {
        m_vkDevCtx.InitDebugReport(true, true);
    }
    
    // Select physical device with compute queue
    vk::DeviceUuidUtils deviceUuid;
    result = m_vkDevCtx.InitPhysicalDevice(
        -1,                                 // deviceId: auto
        deviceUuid,                         // UUID: auto
        VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT,  // Queue types
        nullptr,                            // No WSI display
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR,  // No decode queues
        0, VK_VIDEO_CODEC_OPERATION_NONE_KHR   // No encode queues
    );
    
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to select physical device: " << result << std::endl;
        return result;
    }
    
    // Print device info and cache vendor for workarounds
    VkPhysicalDeviceProperties props;
    m_vkDevCtx.GetPhysicalDeviceProperties(m_vkDevCtx.getPhysicalDevice(), &props);
    m_vendorID = props.vendorID;
    std::cout << "[INFO] Physical device: " << props.deviceName << std::endl;
    
    // Check extension support
    uint32_t extCount = 0;
    m_vkDevCtx.EnumerateDeviceExtensionProperties(m_vkDevCtx.getPhysicalDevice(), nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    m_vkDevCtx.EnumerateDeviceExtensionProperties(m_vkDevCtx.getPhysicalDevice(), nullptr, &extCount, extensions.data());
    
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0) {
            m_drmModifierSupported = true;
        } else if (strcmp(ext.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
            m_dmaBufSupported = true;
        } else if (strcmp(ext.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            m_externalMemorySupported = true;
        } else if (strcmp(ext.extensionName, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME) == 0) {
            m_ycbcrSupported = true;
        }
    }
    
    std::cout << "[INFO] VK_EXT_image_drm_format_modifier: " 
              << (m_drmModifierSupported ? "supported" : "NOT supported") << std::endl;
    std::cout << "[INFO] VK_EXT_external_memory_dma_buf: " 
              << (m_dmaBufSupported ? "supported" : "NOT supported") << std::endl;
    std::cout << "[INFO] VK_KHR_sampler_ycbcr_conversion: "
              << (m_ycbcrSupported ? "supported" : "NOT supported") << std::endl;
    
    if (!m_drmModifierSupported) {
        std::cerr << "[ERROR] VK_EXT_image_drm_format_modifier not supported!" << std::endl;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    
    // Create logical device
    result = m_vkDevCtx.CreateVulkanDevice(
        0,                              // numDecodeQueues
        0,                              // numEncodeQueues
        VK_VIDEO_CODEC_OPERATION_NONE_KHR,
        true,                           // createTransferQueue
        false,                          // createGraphicsQueue
        false,                          // createPresentQueue
        true                            // createComputeQueue
    );
    
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to create logical device: " << result << std::endl;
        return result;
    }
    
    // Get queue info
    m_queue = m_vkDevCtx.GetComputeQueue();
    m_queueFamilyIndex = m_vkDevCtx.GetComputeQueueFamilyIdx();
    
    // Create command pool
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = m_queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    
    result = m_vkDevCtx.CreateCommandPool(m_vkDevCtx.getDevice(), &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to create command pool: " << result << std::endl;
        return result;
    }
    
    return VK_SUCCESS;
}

//=============================================================================
// Query DRM Modifiers
//=============================================================================

std::vector<DrmModifierInfo> DrmFormatModTest::queryDrmModifiers(VkFormat format) const {
    std::vector<DrmModifierInfo> result;
    
    // First query to get count
    VkDrmFormatModifierPropertiesListEXT modifierList{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
    };
    
    VkFormatProperties2 formatProps2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    formatProps2.pNext = &modifierList;
    
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(m_vkDevCtx.getPhysicalDevice(), format, &formatProps2);
    
    if (modifierList.drmFormatModifierCount == 0) {
        return result;
    }
    
    // Second query to get properties
    std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierList.drmFormatModifierCount);
    modifierList.pDrmFormatModifierProperties = modifierProps.data();
    
    m_vkDevCtx.GetPhysicalDeviceFormatProperties2(m_vkDevCtx.getPhysicalDevice(), format, &formatProps2);
    
    for (const auto& mod : modifierProps) {
        DrmModifierInfo info;
        info.modifier = mod.drmFormatModifier;
        info.planeCount = mod.drmFormatModifierPlaneCount;
        info.features = mod.drmFormatModifierTilingFeatures;
        info.features2 = 0;  // Would need VkDrmFormatModifierPropertiesList2EXT for VK1.3+
        result.push_back(info);
    }
    
    return result;
}

//=============================================================================
// Format Support Check
//=============================================================================

bool DrmFormatModTest::isFormatSupported(VkFormat format) const {
    auto modifiers = queryDrmModifiers(format);
    return !modifiers.empty();
}

//=============================================================================
// List Supported Formats
//=============================================================================

void DrmFormatModTest::listSupportedFormats() const {
    std::cout << "\n=== Supported Formats with DRM Modifiers ===" << std::endl;
    
    auto formats = getAllFormats();
    int supported = 0;
    
    for (const auto& fmt : formats) {
        auto modifiers = queryDrmModifiers(fmt.vkFormat);
        if (!modifiers.empty()) {
            std::cout << "\n" << fmt.name << " (" << vkFormatToString(fmt.vkFormat) << ")"
                      << " - " << modifiers.size() << " modifier(s):" << std::endl;
            
            for (const auto& mod : modifiers) {
                char modStr[64];
                modifierToString(mod.modifier, modStr, sizeof(modStr));
                std::cout << "    " << modStr 
                          << " planes=" << mod.planeCount
                          << " features=" << mod.featuresToString()
                          << std::endl;
            }
            supported++;
        }
    }
    
    std::cout << "\nTotal: " << supported << "/" << formats.size() << " formats supported" << std::endl;
}

//=============================================================================
// Run All Tests
//=============================================================================

std::vector<TestResult> DrmFormatModTest::runAllTests() {
    std::vector<TestResult> results;
    
    // Get formats to test based on config
    std::vector<FormatInfo> formats;
    if (!m_config.specificFormat.empty()) {
        const FormatInfo* fmt = getFormatByName(m_config.specificFormat.c_str());
        if (fmt) {
            formats.push_back(*fmt);
        } else {
            std::cerr << "[ERROR] Unknown format: " << m_config.specificFormat << std::endl;
            return results;
        }
    } else if (m_config.rgbOnly) {
        formats = getRgbFormats();
    } else if (m_config.ycbcrOnly) {
        formats = getYcbcrFormats();
    } else {
        formats = getAllFormats();
    }
    
    std::cout << "\n=== Running DRM Format Modifier Tests ===" << std::endl;
    std::cout << "Testing " << formats.size() << " format(s)";
    switch (m_config.compression) {
        case CompressionMode::Enable:  std::cout << " [compression=ENABLED]"; break;
        case CompressionMode::Disable: std::cout << " [compression=DISABLED]"; break;
        default: break;
    }
    std::cout << std::endl;
    
    for (const auto& fmt : formats) {
        if (!isFormatSupported(fmt.vkFormat)) {
            TestResult r;
            r.testName = std::string("SKIP_") + fmt.name;
            r.status = TestStatus::SKIP;
            r.message = "No DRM modifiers available";
            results.push_back(r);
            
            if (m_config.verbose) {
                std::cout << "[SKIP] " << fmt.name << ": No DRM modifiers" << std::endl;
            }
            continue;
        }
        
        // TC1: Format Query Test
        results.push_back(runFormatQueryTest(fmt));
        
        // TC2: Image Creation with LINEAR
        // Skip LINEAR when video usage is requested — NVDEC/NVENC require tiled memory.
        if (!(m_config.videoEncode || m_config.videoDecode)) {
            results.push_back(runImageCreateTest(fmt, true));
        } else if (m_config.verbose) {
            std::cout << "[SKIP] TC2_Create_" << fmt.name
                      << "_LINEAR: NVDEC/NVENC require tiled (not linear)" << std::endl;
        }
        
        // TC2: Image Creation with OPTIMAL/TILED (if not linear-only)
        if (!m_config.linearOnly) {
            results.push_back(runImageCreateTest(fmt, false));
        }
        
        // TC3: Export/Import with LINEAR
        // Skip LINEAR when video usage is requested — NVDEC/NVENC require tiled memory.
        if (!(m_config.videoEncode || m_config.videoDecode)) {
            results.push_back(runExportImportTest(fmt, true));
        }
        
        // TC3: Export/Import with OPTIMAL (uncompressed block-linear)
        if (!m_config.linearOnly) {
            results.push_back(runExportImportTest(fmt, false));
        }
        
        // TC4: Export/Import with COMPRESSED block-linear
        // The driver reports both compressed and uncompressed modifiers
        // when __GL_CompressedFormatModifiers includes GPU_SUPPORTED (bit 0).
        // This test selects a compressed modifier from the advertised list.
        // Runs when:
        //   - Not --linear-only
        //   - Not --compression disable (explicitly disabled)
        //   - Compressed modifiers are advertised by the driver
        if (!m_config.linearOnly && m_config.compression != CompressionMode::Disable) {
            auto mods = queryDrmModifiers(fmt.vkFormat);
            bool hasCompressed = false;
            for (const auto& mod : mods) {
                if (mod.isLinear()) continue;
                if (mod.isCompressed()) {
                    hasCompressed = true;
                }
            }
            if (hasCompressed) {
                results.push_back(runExportImportTest(fmt, false, true));
                
                // Log modifier breakdown in verbose mode
                if (m_config.verbose) {
                    uint32_t numComp = 0, numUncomp = 0;
                    for (const auto& mod : mods) {
                        if (mod.isLinear()) continue;
                        if (mod.isCompressed()) numComp++;
                        else numUncomp++;
                    }
                    std::cout << "    Modifiers: " << numComp << " compressed + " 
                              << numUncomp << " uncompressed + 1 LINEAR = " 
                              << mods.size() << " total" << std::endl;
                }
            } else if (m_config.compression == CompressionMode::Enable && m_config.verbose) {
                // User explicitly requested compression but none available for this format
                std::cout << "[INFO] " << fmt.name 
                          << ": No compressed modifiers advertised"
                          << " (set __GL_CompressedFormatModifiers=0x101 or use --compression enable)"
                          << std::endl;
            }
        }
        
        // TC5: Video format query (vkGetPhysicalDeviceVideoFormatPropertiesKHR)
        if (m_config.videoEncode) {
            results.push_back(runVideoFormatQueryTest(fmt, true));
        }
        if (m_config.videoDecode) {
            results.push_back(runVideoFormatQueryTest(fmt, false));
        }
        
        // TC6: Plane layout verification (export → query layouts → import → compare)
        if (m_config.videoEncode || m_config.videoDecode) {
            if (!m_config.linearOnly) {
                results.push_back(runPlaneLayoutTest(fmt, false));
            }
        } else {
            results.push_back(runPlaneLayoutTest(fmt, true));
            if (!m_config.linearOnly) {
                results.push_back(runPlaneLayoutTest(fmt, false));
            }
        }
    }
    
    printTestSummary(results, m_config.verbose);
    
    return results;
}

//=============================================================================
// Format Query Test
//=============================================================================

TestResult DrmFormatModTest::runFormatQueryTest(const FormatInfo& format) {
    TestResult result;
    result.testName = std::string("TC1_Query_") + format.name;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    auto modifiers = queryDrmModifiers(format.vkFormat);
    
    if (modifiers.empty()) {
        result.status = TestStatus::SKIP;
        result.message = "No modifiers available";
        return result;
    }
    
    // Validate modifiers
    bool hasLinear = false;
    bool valid = true;
    std::stringstream details;
    
    for (const auto& mod : modifiers) {
        if (mod.isLinear()) {
            hasLinear = true;
        }
        
        // Validate plane count
        if (mod.planeCount == 0 || mod.planeCount > 4) {
            valid = false;
            details << "Invalid plane count " << mod.planeCount << " for modifier 0x"
                    << std::hex << mod.modifier << std::dec << "; ";
        }
        
        // Validate features (should have at least some capability)
        if (mod.features == 0) {
            valid = false;
            details << "No features for modifier 0x" << std::hex << mod.modifier << std::dec << "; ";
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (valid) {
        result.status = TestStatus::PASS;
        result.message = std::to_string(modifiers.size()) + " modifiers found" + 
                         (hasLinear ? " (LINEAR supported)" : "");
    } else {
        result.status = TestStatus::FAIL;
        result.message = details.str();
    }
    
    if (m_config.verbose) {
        std::cout << "[" << testStatusToString(result.status) << "] " 
                  << result.testName << ": " << result.message << std::endl;
        for (const auto& mod : modifiers) {
            std::cout << "    " << mod.modifierToString() 
                      << " planes=" << mod.planeCount
                      << " features=" << mod.featuresToString() << std::endl;
        }
    }
    
    return result;
}

//=============================================================================
// Image Creation Test
//=============================================================================

TestResult DrmFormatModTest::runImageCreateTest(const FormatInfo& format, bool useLinear) {
    TestResult result;
    result.testName = std::string("TC2_Create_") + format.name + 
                      (useLinear ? "_LINEAR" : "_OPTIMAL");
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Find appropriate modifier
    auto modifiers = queryDrmModifiers(format.vkFormat);
    uint64_t targetModifier = DRM_FORMAT_MOD_INVALID;
    
    if (useLinear) {
        for (const auto& mod : modifiers) {
            if (mod.isLinear()) {
                targetModifier = mod.modifier;
                break;
            }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            result.status = TestStatus::SKIP;
            result.message = "LINEAR modifier not available";
            return result;
        }
    } else {
        // Find first non-linear modifier
        for (const auto& mod : modifiers) {
            if (!mod.isLinear()) {
                targetModifier = mod.modifier;
                break;
            }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            if (m_config.videoEncode || m_config.videoDecode) {
                // Video requires tiled — cannot fall back to LINEAR
                result.status = TestStatus::SKIP;
                result.message = "No tiled modifier available (video requires tiled)";
                return result;
            }
            // Fall back to linear if no tiled modifiers
            targetModifier = DRM_FORMAT_MOD_LINEAR;
        }
    }
    
    // Create image
    VkSharedBaseObj<VkImageResource> image;
    VkResult vkResult = createExportableImage(format, targetModifier, image);
    
    auto end = std::chrono::high_resolution_clock::now();
    result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (vkResult != VK_SUCCESS) {
        result.status = TestStatus::FAIL;
        result.message = "vkCreateImage failed: " + std::to_string(vkResult);
    } else {
        // Query actual modifier used
        uint64_t actualModifier = 0;
        vkResult = queryImageDrmModifier(image->GetImage(), &actualModifier);
        
        if (vkResult == VK_SUCCESS) {
            char modStr[64];
            modifierToString(actualModifier, modStr, sizeof(modStr));
            result.status = TestStatus::PASS;
            result.message = std::string("Created with modifier ") + modStr;
        } else {
            result.status = TestStatus::PASS;
            result.message = "Created (modifier query N/A)";
        }
    }
    
    if (m_config.verbose || result.status == TestStatus::FAIL) {
        std::cout << "[" << testStatusToString(result.status) << "] " 
                  << result.testName << ": " << result.message << std::endl;
    }
    
    return result;
}

//=============================================================================
// Export/Import Test
//=============================================================================

TestResult DrmFormatModTest::runExportImportTest(const FormatInfo& format, bool useLinear, bool useCompressed) {
    const char* modeName = useLinear ? "LINEAR" : (useCompressed ? "COMPRESSED" : "OPTIMAL");
    TestResult result;
    result.testName = std::string("TC3_ExportImport_") + format.name + "_" + modeName;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Find modifier
    auto modifiers = queryDrmModifiers(format.vkFormat);
    uint64_t targetModifier = useLinear ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID;
    
    if (useLinear) {
        bool found = false;
        for (const auto& mod : modifiers) {
            if (mod.isLinear()) {
                found = true;
                break;
            }
        }
        if (!found) {
            result.status = TestStatus::SKIP;
            result.message = "LINEAR modifier not available";
            return result;
        }
        // Intel (vendor 0x8086): single-plane LINEAR DMA-BUF import returns
        // VK_ERROR_INVALID_EXTERNAL_HANDLE; multi-plane (e.g. NV12, P010) works.
        const uint32_t planeCount = format.planeCount > 0 ? format.planeCount : 1;
        if (planeCount == 1 && m_vendorID == 0x8086) {
            result.status = TestStatus::SKIP;
            result.message = "Intel: single-plane LINEAR DMA-BUF import returns VK_ERROR_INVALID_EXTERNAL_HANDLE (driver limitation)";
            return result;
        }
    } else if (useCompressed) {
        // Find first compressed block-linear modifier
        for (const auto& mod : modifiers) {
            if (!mod.isLinear() && mod.isCompressed()) {
                targetModifier = mod.modifier;
                break;
            }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            result.status = TestStatus::SKIP;
            result.message = "No compressed modifier available";
            return result;
        }
    } else {
        // Find first uncompressed block-linear modifier
        for (const auto& mod : modifiers) {
            if (!mod.isLinear() && !mod.isCompressed()) {
                targetModifier = mod.modifier;
                break;
            }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            if (m_config.videoEncode || m_config.videoDecode) {
                // Video requires tiled — cannot fall back to LINEAR
                result.status = TestStatus::SKIP;
                result.message = "No tiled modifier available (video requires tiled)";
                return result;
            }
            targetModifier = DRM_FORMAT_MOD_LINEAR;
        }
    }
    
    // Create exportable image
    VkSharedBaseObj<VkImageResource> srcImage;
    VkResult vkResult = createExportableImage(format, targetModifier, srcImage);
    
    if (vkResult != VK_SUCCESS) {
        result.status = TestStatus::FAIL;
        result.message = "Source image creation failed: " + std::to_string(vkResult);
        return result;
    }
    
    // Export DMA-BUF FD
    int dmaBufFd = -1;
    vkResult = exportDmaBufFd(srcImage, &dmaBufFd);
    
    if (vkResult != VK_SUCCESS || dmaBufFd < 0) {
        result.status = TestStatus::FAIL;
        result.message = "Export failed: " + std::to_string(vkResult);
        return result;
    }
    
    if (m_config.verbose) {
        std::cout << "    Exported DMA-BUF FD: " << dmaBufFd << std::endl;
    }
    
    // Query actual modifier
    uint64_t actualModifier = targetModifier;
    queryImageDrmModifier(srcImage->GetImage(), &actualModifier);
    
    // Get memory info for import
    VkMemoryRequirements memReqs;
    m_vkDevCtx.GetImageMemoryRequirements(m_vkDevCtx.getDevice(), srcImage->GetImage(), &memReqs);
    
    // Skip import if exportOnly mode
    if (m_config.exportOnly) {
        close(dmaBufFd);
        auto end = std::chrono::high_resolution_clock::now();
        result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
        result.status = TestStatus::PASS;
        result.message = "Export successful (import skipped)";
        return result;
    }
    
    // Query actual plane layouts from the exported image for import
    VkSubresourceLayout exportedPlaneLayouts[4] = {};
    uint32_t planeCount = format.planeCount > 0 ? format.planeCount : 1;
    
    for (uint32_t p = 0; p < planeCount; p++) {
        VkImageSubresource subres{};
        if (planeCount > 1) {
            // For multiplanar DRM modifier images, query MEMORY_PLANE aspects
            subres.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p;
        } else {
            subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        subres.mipLevel = 0;
        subres.arrayLayer = 0;
        m_vkDevCtx.GetImageSubresourceLayout(m_vkDevCtx.getDevice(), 
            srcImage->GetImage(), &subres, &exportedPlaneLayouts[p]);
    }
    
    // Import back
    VkSharedBaseObj<VkImageResource> importedImage;
    vkResult = importDmaBufImage(format, dmaBufFd, memReqs.size, actualModifier, 
                                  memReqs.memoryTypeBits, exportedPlaneLayouts,
                                  planeCount, importedImage);
    
    // Note: dmaBufFd is consumed by import (ownership transferred to Vulkan)
    
    auto end = std::chrono::high_resolution_clock::now();
    result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (vkResult != VK_SUCCESS) {
        result.status = TestStatus::FAIL;
        result.message = "Import failed: " + std::to_string(vkResult);
    } else {
        char modStr[64];
        modifierToString(actualModifier, modStr, sizeof(modStr));
        result.status = TestStatus::PASS;
        result.message = std::string("Round-trip successful with ") + modStr;
        destroyImportedImage(importedImage);
    }
    
    if (m_config.verbose || result.status == TestStatus::FAIL) {
        std::cout << "[" << testStatusToString(result.status) << "] " 
                  << result.testName << ": " << result.message << std::endl;
    }
    
    return result;
}

//=============================================================================
// Create Exportable Image
//=============================================================================

VkResult DrmFormatModTest::createExportableImage(
    const FormatInfo& format,
    uint64_t drmModifier,
    VkSharedBaseObj<VkImageResource>& outImage)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format.vkFormat;
    imageInfo.extent = {m_config.testImageWidth, m_config.testImageHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;  // Will be overridden
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    
    // Add sampled usage for non-YCbCr formats
    if (!format.isYcbcr) {
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    
    // Add video usage flags when requested.
    // VIDEO_ENCODE_SRC / VIDEO_DECODE_DST are the usage bits that the video HW needs.
    // EXTENDED_USAGE + MUTABLE_FORMAT allow per-plane STORAGE views on multi-planar images.
    // VIDEO_PROFILE_INDEPENDENT avoids needing a VkVideoProfileListInfoKHR at image creation.
    if (m_config.videoEncode || m_config.videoDecode) {
        imageInfo.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
                        |  VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                        |  VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (m_config.videoEncode) {
            imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
        }
        if (m_config.videoDecode) {
            imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
        }
        
        if (m_config.verbose) {
            std::cout << "  [video] usage=0x" << std::hex << imageInfo.usage
                      << " flags=0x" << imageInfo.flags << std::dec << std::endl;
        }
    }
    
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    // Note: We intentionally do NOT set VK_IMAGE_CREATE_DISJOINT_BIT even if the
    // format supports it (VK_FORMAT_FEATURE_DISJOINT_BIT). This is because:
    // 1. Disjoint images require per-plane memory binding via VkBindImagePlaneMemoryInfo
    // 2. VkImageResource::CreateExportable uses vkBindImageMemory which doesn't support this
    // 3. Using DISJOINT_BIT without proper per-plane binding violates VUID-VkBindImageMemoryInfo-image-07736
    //
    // For non-disjoint multi-planar images, all planes share a single memory allocation
    // and can be bound with a single vkBindImageMemory call, which is what we support.
    (void)format; // Suppress unused warning - we don't set DISJOINT_BIT
    
    return VkImageResource::CreateExportable(
        &m_vkDevCtx,
        &imageInfo,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        drmModifier,
        outImage
    );
}

//=============================================================================
// TC5: Video Format Query Test
// Calls vkGetPhysicalDeviceVideoFormatPropertiesKHR with encode/decode usage
// to verify which formats/tiling the driver reports for video.
//=============================================================================

TestResult DrmFormatModTest::runVideoFormatQueryTest(const FormatInfo& format, bool encode) {
    TestResult result;
    result.testName = std::string("TC5_VideoFmtQuery_") + format.name +
                      (encode ? "_ENCODE" : "_DECODE");

    VkImageUsageFlags videoUsage = encode
        ? VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
        : VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;

    // Build a dummy H.264 4:2:0 8-bit profile for the query.
    // We need at least one profile in the list.
    VkVideoDecodeH264ProfileInfoKHR h264DecProfile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    h264DecProfile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264DecProfile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

    VkVideoEncodeH264ProfileInfoKHR h264EncProfile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    h264EncProfile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;

    VkVideoProfileInfoKHR profileInfo{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    profileInfo.videoCodecOperation = encode
        ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR
        : VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.pNext = encode ? (void*)&h264EncProfile : (void*)&h264DecProfile;

    VkVideoProfileListInfoKHR profileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profileList.profileCount = 1;
    profileList.pProfiles = &profileInfo;

    VkPhysicalDeviceVideoFormatInfoKHR formatInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    formatInfo.imageUsage = videoUsage;
    formatInfo.pNext = &profileList;

    // First call: get count
    uint32_t formatCount = 0;
    VkResult vkResult = m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(
        m_vkDevCtx.getPhysicalDevice(), &formatInfo, &formatCount, nullptr);

    if (vkResult != VK_SUCCESS || formatCount == 0) {
        result.status = TestStatus::SKIP;
        result.message = "No video format properties returned (result=" +
                         std::to_string(vkResult) + " count=" + std::to_string(formatCount) + ")";
        if (m_config.verbose) {
            std::cout << "[SKIP] " << result.testName << ": " << result.message << std::endl;
        }
        return result;
    }

    // Second call: get properties
    std::vector<VkVideoFormatPropertiesKHR> formatProps(formatCount);
    for (auto& fp : formatProps) {
        fp.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        fp.pNext = nullptr;
    }
    vkResult = m_vkDevCtx.GetPhysicalDeviceVideoFormatPropertiesKHR(
        m_vkDevCtx.getPhysicalDevice(), &formatInfo, &formatCount, formatProps.data());

    if (vkResult != VK_SUCCESS) {
        result.status = TestStatus::FAIL;
        result.message = "GetPhysicalDeviceVideoFormatPropertiesKHR failed: " +
                         std::to_string(vkResult);
        return result;
    }

    // Check if our target format is in the returned list
    bool foundFormat = false;
    std::stringstream details;
    details << formatCount << " video formats returned:";

    for (uint32_t i = 0; i < formatCount; i++) {
        const auto& fp = formatProps[i];
        const char* fmtName = vkFormatToString(fp.format);
        const char* tilingName = (fp.imageTiling == VK_IMAGE_TILING_OPTIMAL) ? "OPTIMAL" :
                                 (fp.imageTiling == VK_IMAGE_TILING_LINEAR) ? "LINEAR" : "DRM_MOD";

        if (m_config.verbose) {
            std::cout << "    [" << i << "] " << fmtName
                      << " tiling=" << tilingName
                      << " usage=0x" << std::hex << fp.imageUsageFlags << std::dec
                      << " flags=0x" << std::hex << fp.imageCreateFlags << std::dec
                      << std::endl;
        }

        if (fp.format == format.vkFormat) {
            foundFormat = true;
        }
    }

    if (foundFormat) {
        result.status = TestStatus::PASS;
        result.message = details.str() + " (target format found)";
    } else {
        result.status = TestStatus::FAIL;
        result.message = details.str() + " (target format " + format.name + " NOT found)";
    }

    if (m_config.verbose || result.failed()) {
        std::cout << "[" << testStatusToString(result.status) << "] "
                  << result.testName << ": " << result.message << std::endl;
    }

    return result;
}

//=============================================================================
// TC6: Plane Layout Verification Test
// Creates an exportable image, queries plane layouts (pitch, offset, size),
// exports as DMA-BUF, imports with same parameters, queries imported layouts,
// and compares them.
//=============================================================================

TestResult DrmFormatModTest::runPlaneLayoutTest(const FormatInfo& format, bool useLinear) {
    TestResult result;
    result.testName = std::string("TC6_PlaneLayout_") + format.name +
                      (useLinear ? "_LINEAR" : "_TILED");

    // Find modifier
    auto modifiers = queryDrmModifiers(format.vkFormat);
    uint64_t targetModifier = DRM_FORMAT_MOD_INVALID;

    if (useLinear) {
        for (const auto& mod : modifiers) {
            if (mod.isLinear()) { targetModifier = mod.modifier; break; }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            result.status = TestStatus::SKIP;
            result.message = "LINEAR modifier not available";
            return result;
        }
    } else {
        for (const auto& mod : modifiers) {
            if (!mod.isLinear() && !mod.isCompressed()) {
                targetModifier = mod.modifier; break;
            }
        }
        if (targetModifier == DRM_FORMAT_MOD_INVALID) {
            result.status = TestStatus::SKIP;
            result.message = "No tiled modifier available";
            return result;
        }
    }

    // Create exportable image
    VkSharedBaseObj<VkImageResource> srcImage;
    VkResult vkResult = createExportableImage(format, targetModifier, srcImage);
    if (vkResult != VK_SUCCESS) {
        result.status = TestStatus::FAIL;
        result.message = "Source image creation failed: " + std::to_string(vkResult);
        return result;
    }

    // Query actual modifier
    uint64_t actualModifier = 0;
    queryImageDrmModifier(srcImage->GetImage(), &actualModifier);

    // Query plane layouts from exported image
    uint32_t planeCount = format.planeCount > 0 ? format.planeCount : 1;
    VkSubresourceLayout exportLayouts[4] = {};

    for (uint32_t p = 0; p < planeCount; p++) {
        VkImageSubresource subres{};
        subres.aspectMask = (planeCount > 1)
            ? static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_PLANE_0_BIT << p)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        subres.mipLevel = 0;
        subres.arrayLayer = 0;

        m_vkDevCtx.GetImageSubresourceLayout(
            m_vkDevCtx.getDevice(), srcImage->GetImage(), &subres, &exportLayouts[p]);
    }

    // Print exported layouts
    if (m_config.verbose) {
        char modStr[64];
        modifierToString(actualModifier, modStr, sizeof(modStr));
        std::cout << "  Export image: modifier=" << modStr
                  << " planes=" << planeCount << std::endl;
        for (uint32_t p = 0; p < planeCount; p++) {
            std::cout << "    Plane " << p
                      << ": offset=" << exportLayouts[p].offset
                      << " size=" << exportLayouts[p].size
                      << " rowPitch=" << exportLayouts[p].rowPitch
                      << " arrayPitch=" << exportLayouts[p].arrayPitch
                      << " depthPitch=" << exportLayouts[p].depthPitch
                      << std::endl;
        }
    }

    // Validate exported layouts
    bool valid = true;
    std::stringstream issues;

    for (uint32_t p = 0; p < planeCount; p++) {
        if (exportLayouts[p].rowPitch == 0) {
            valid = false;
            issues << "Plane " << p << " rowPitch=0; ";
        }
        if (p > 0 && exportLayouts[p].offset == 0 && !useLinear) {
            // Tiled planes should have non-zero offsets for planes > 0
            // (unless interleaved, which NV12 is not)
            valid = false;
            issues << "Plane " << p << " offset=0 for tiled; ";
        }
        if (p > 0 && exportLayouts[p].offset <= exportLayouts[p-1].offset) {
            valid = false;
            issues << "Plane " << p << " offset not increasing; ";
        }
    }

    // Export DMA-BUF FD
    int dmaBufFd = -1;
    vkResult = exportDmaBufFd(srcImage, &dmaBufFd);
    if (vkResult != VK_SUCCESS || dmaBufFd < 0) {
        result.status = TestStatus::FAIL;
        result.message = "DMA-BUF export failed: " + std::to_string(vkResult);
        return result;
    }

    // Import with same parameters
    VkSharedBaseObj<VkImageResource> dstImage;
    VkDeviceSize allocSize = srcImage->GetImageDeviceMemorySize();
    uint32_t memTypeBits = 0; // importDmaBufImage queries compatible types

    vkResult = importDmaBufImage(format, dmaBufFd, allocSize, actualModifier,
                                  memTypeBits, exportLayouts, planeCount, dstImage);
    close(dmaBufFd);

    if (vkResult != VK_SUCCESS || !dstImage) {
        result.status = TestStatus::FAIL;
        result.message = "DMA-BUF import failed: vkResult=" + std::to_string(vkResult)
                       + " dstImage=" + (dstImage ? "valid" : "NULL");
        if (m_config.verbose) {
            std::cout << "[FAIL] " << result.testName << ": " << result.message << std::endl;
        }
        return result;
    }

    // Query imported image layouts
    VkSubresourceLayout importLayouts[4] = {};
    for (uint32_t p = 0; p < planeCount; p++) {
        VkImageSubresource subres{};
        subres.aspectMask = (planeCount > 1)
            ? static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_PLANE_0_BIT << p)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        subres.mipLevel = 0;
        subres.arrayLayer = 0;

        m_vkDevCtx.GetImageSubresourceLayout(
            m_vkDevCtx.getDevice(), dstImage->GetImage(), &subres, &importLayouts[p]);
    }

    // Compare export vs import layouts
    if (m_config.verbose) {
        std::cout << "  Import image: planes=" << planeCount << std::endl;
        for (uint32_t p = 0; p < planeCount; p++) {
            std::cout << "    Plane " << p
                      << ": offset=" << importLayouts[p].offset
                      << " size=" << importLayouts[p].size
                      << " rowPitch=" << importLayouts[p].rowPitch
                      << std::endl;
        }
    }

    for (uint32_t p = 0; p < planeCount; p++) {
        if (exportLayouts[p].offset != importLayouts[p].offset) {
            valid = false;
            issues << "Plane " << p << " offset mismatch: export="
                   << exportLayouts[p].offset << " import=" << importLayouts[p].offset << "; ";
        }
        if (exportLayouts[p].rowPitch != importLayouts[p].rowPitch) {
            valid = false;
            issues << "Plane " << p << " rowPitch mismatch: export="
                   << exportLayouts[p].rowPitch << " import=" << importLayouts[p].rowPitch << "; ";
        }
        if (exportLayouts[p].size != importLayouts[p].size) {
            valid = false;
            issues << "Plane " << p << " size mismatch: export="
                   << exportLayouts[p].size << " import=" << importLayouts[p].size << "; ";
        }
    }

    // Clean up imported image (non-owning wrapper, must destroy raw handles)
    destroyImportedImage(dstImage);

    if (valid) {
        result.status = TestStatus::PASS;
        result.message = std::to_string(planeCount) + " planes verified (export == import)";
    } else {
        result.status = TestStatus::FAIL;
        result.message = issues.str();
    }

    if (m_config.verbose || result.failed()) {
        std::cout << "[" << testStatusToString(result.status) << "] "
                  << result.testName << ": " << result.message << std::endl;
    }

    return result;
}

//=============================================================================
// Export DMA-BUF FD
//=============================================================================

VkResult DrmFormatModTest::exportDmaBufFd(
    const VkSharedBaseObj<VkImageResource>& image,
    int* outFd)
{
    VkMemoryGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    getFdInfo.memory = image->GetDeviceMemory();
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    
    return m_vkDevCtx.GetMemoryFdKHR(m_vkDevCtx.getDevice(), &getFdInfo, outFd);
}

//=============================================================================
// Import DMA-BUF Image
//=============================================================================

VkResult DrmFormatModTest::importDmaBufImage(
    const FormatInfo& format,
    int fd,
    VkDeviceSize size,
    uint64_t drmModifier,
    uint32_t memoryTypeBits,
    const VkSubresourceLayout* srcPlaneLayouts,
    uint32_t planeCount,
    VkSharedBaseObj<VkImageResource>& outImage)
{
    // For import, we need to use VkImageDrmFormatModifierExplicitCreateInfoEXT
    // and import the memory with VkImportMemoryFdInfoKHR
    
    // Step 1: Create image with explicit DRM modifier
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format.vkFormat;
    imageInfo.extent = {m_config.testImageWidth, m_config.testImageHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!format.isYcbcr) {
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    // External memory info
    VkExternalMemoryImageCreateInfo extMemInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    
    const bool isLinear = (drmModifier == DRM_FORMAT_MOD_LINEAR);
    
    // For LINEAR modifiers: use explicit layout with queried plane offsets/pitches.
    // For block-linear (tiled) modifiers: use list mode — the driver determines
    // the internal layout and the imported memory must match the export's tiling.
    VkSubresourceLayout planeLayouts[4] = {};
    VkImageDrmFormatModifierExplicitCreateInfoEXT drmExplicit{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT
    };
    VkImageDrmFormatModifierListCreateInfoEXT drmList{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT
    };
    
    if (isLinear) {
        // Explicit layout for LINEAR — pass exact offsets/pitches from export
        for (uint32_t p = 0; p < planeCount; p++) {
            planeLayouts[p].offset = srcPlaneLayouts[p].offset;
            planeLayouts[p].size = 0;  // Must be 0 per spec
            planeLayouts[p].rowPitch = srcPlaneLayouts[p].rowPitch;
            planeLayouts[p].arrayPitch = 0;
            planeLayouts[p].depthPitch = 0;
        }
        drmExplicit.drmFormatModifier = drmModifier;
        drmExplicit.drmFormatModifierPlaneCount = planeCount;
        drmExplicit.pPlaneLayouts = planeLayouts;
        extMemInfo.pNext = &drmExplicit;
    } else {
        // List mode for block-linear — driver picks the matching layout
        drmList.drmFormatModifierCount = 1;
        drmList.pDrmFormatModifiers = &drmModifier;
        extMemInfo.pNext = &drmList;
    }
    
    imageInfo.pNext = &extMemInfo;
    
    VkDevice device = m_vkDevCtx.getDevice();
    VkImage image = VK_NULL_HANDLE;
    
    VkResult result = m_vkDevCtx.CreateImage(device, &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) {
        close(fd);  // We own the fd, clean up on failure
        return result;
    }
    
    // Get memory requirements
    VkMemoryRequirements memReqs;
    m_vkDevCtx.GetImageMemoryRequirements(device, image, &memReqs);
    
    // Find suitable memory type
    VkPhysicalDeviceMemoryProperties memProps;
    m_vkDevCtx.GetPhysicalDeviceMemoryProperties(m_vkDevCtx.getPhysicalDevice(), &memProps);
    
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    
    if (memoryTypeIndex == UINT32_MAX) {
        m_vkDevCtx.DestroyImage(device, image, nullptr);
        close(fd);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    
    // Import memory
    VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = fd;  // Vulkan takes ownership
    
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = m_vkDevCtx.AllocateMemory(device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        m_vkDevCtx.DestroyImage(device, image, nullptr);
        // fd ownership transferred to Vulkan even on failure
        return result;
    }
    
    // Bind memory
    result = m_vkDevCtx.BindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) {
        m_vkDevCtx.FreeMemory(device, memory, nullptr);
        m_vkDevCtx.DestroyImage(device, image, nullptr);
        return result;
    }
    
    // Wrap in VkImageResource. CreateFromExternal is non-owning — it doesn't
    // store the memory handle. Track raw handles in m_importedHandles for
    // cleanup via destroyImportedImage().
    VkImageCreateInfo wrapCI = imageInfo;
    wrapCI.tiling = isLinear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    result = VkImageResource::CreateFromExternal(&m_vkDevCtx, image, memory,
                                                  &wrapCI, outImage);
    if (result != VK_SUCCESS) {
        m_vkDevCtx.FreeMemory(device, memory, nullptr);
        m_vkDevCtx.DestroyImage(device, image, nullptr);
    } else {
        m_importedHandles.push_back({image, memory});
    }
    
    return result;
}

void DrmFormatModTest::destroyImportedImage(VkSharedBaseObj<VkImageResource>& image) {
    if (!image) return;
    VkDevice device = m_vkDevCtx.getDevice();
    VkImage img = image->GetImage();
    image = nullptr;  // Release the wrapper first
    
    // Find and destroy the raw handles tracked during import
    for (auto it = m_importedHandles.begin(); it != m_importedHandles.end(); ++it) {
        if (it->image == img) {
            m_vkDevCtx.DestroyImage(device, it->image, nullptr);
            m_vkDevCtx.FreeMemory(device, it->memory, nullptr);
            m_importedHandles.erase(it);
            return;
        }
    }
}

//=============================================================================
// Query Image DRM Modifier
//=============================================================================

VkResult DrmFormatModTest::queryImageDrmModifier(VkImage image, uint64_t* outModifier) {
    VkImageDrmFormatModifierPropertiesEXT modProps{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT
    };
    
    PFN_vkGetImageDrmFormatModifierPropertiesEXT pfnGetDrmMod =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)m_vkDevCtx.GetDeviceProcAddr(
            m_vkDevCtx.getDevice(), "vkGetImageDrmFormatModifierPropertiesEXT");
    
    if (!pfnGetDrmMod) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    
    VkResult result = pfnGetDrmMod(m_vkDevCtx.getDevice(), image, &modProps);
    if (result == VK_SUCCESS && outModifier) {
        *outModifier = modProps.drmFormatModifier;
    }
    
    return result;
}

//=============================================================================
// Format Support Status to String
//=============================================================================

const char* formatSupportStatusToString(FormatSupportStatus status) {
    switch (status) {
        case FormatSupportStatus::SUPPORTED: return "SUPPORTED";
        case FormatSupportStatus::NOT_SUPPORTED: return "NOT_SUPPORTED";
        case FormatSupportStatus::VIDEO_DRM_FAIL: return "VIDEO_DRM_FAIL";
        case FormatSupportStatus::LINEAR_ONLY: return "LINEAR_ONLY";
        case FormatSupportStatus::EXPORT_FAIL: return "EXPORT_FAIL";
        case FormatSupportStatus::IMPORT_FAIL: return "IMPORT_FAIL";
        case FormatSupportStatus::UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

//=============================================================================
// Vulkan Video Format Support Query
//=============================================================================

bool DrmFormatModTest::isVulkanVideoDecodeFormat(VkFormat format) const {
    for (const auto& f : m_videoDecodeFormats) {
        if (f == format) return true;
    }
    return false;
}

bool DrmFormatModTest::isVulkanVideoEncodeFormat(VkFormat format) const {
    for (const auto& f : m_videoEncodeFormats) {
        if (f == format) return true;
    }
    return false;
}

std::vector<VkFormat> DrmFormatModTest::getVulkanVideoFormats() const {
    // Common video formats used by Vulkan Video (decode/encode)
    // These are the YCbCr formats typically used for 8/10/12 bit video
    static const std::vector<VkFormat> videoFormats = {
        // 8-bit 4:2:0 (most common)
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
        
        // 10-bit 4:2:0
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
        
        // 12-bit 4:2:0
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,
        VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,
        
        // 16-bit 4:2:0
        VK_FORMAT_G16_B16R16_2PLANE_420_UNORM,
        VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM,
        
        // 8-bit 4:2:2
        VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,
        VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM,
        
        // 10-bit 4:2:2
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16,
        VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16,
        
        // 12-bit 4:2:2
        VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16,
        
        // 8-bit 4:4:4
        VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM,
    };
    return videoFormats;
}

//=============================================================================
// Generate Format Report
//=============================================================================

std::vector<FormatSupportEntry> DrmFormatModTest::generateFormatReport() {
    std::vector<FormatSupportEntry> report;
    
    // Get all formats to test
    std::vector<FormatInfo> formats;
    if (m_config.rgbOnly) {
        formats = getRgbFormats();
    } else if (m_config.ycbcrOnly || m_config.videoOnly) {
        formats = getYcbcrFormats();
    } else {
        formats = getAllFormats();
    }
    
    // Get Vulkan Video format list
    auto videoFormats = getVulkanVideoFormats();
    
    std::cout << "\n=== Generating Format Support Report ===" << std::endl;
    std::cout << "Testing " << formats.size() << " formats..." << std::endl;
    
    for (const auto& fmt : formats) {
        FormatSupportEntry entry;
        entry.format = fmt;
        
        // Check if this is a Vulkan Video format
        for (const auto& vf : videoFormats) {
            if (vf == fmt.vkFormat) {
                entry.vulkanVideoSupport = true;
                break;
            }
        }
        
        // Query DRM modifiers
        auto modifiers = queryDrmModifiers(fmt.vkFormat);
        entry.modifierCount = modifiers.size();
        
        if (modifiers.empty()) {
            // No DRM modifier support
            if (entry.vulkanVideoSupport) {
                entry.status = FormatSupportStatus::VIDEO_DRM_FAIL;
                entry.notes = "Vulkan Video format but NO DRM modifier support!";
            } else {
                entry.status = FormatSupportStatus::NOT_SUPPORTED;
                entry.notes = "No DRM modifiers available";
            }
        } else {
            // Check for LINEAR and tiled modifiers
            for (const auto& mod : modifiers) {
                if (mod.isLinear()) {
                    entry.hasLinear = true;
                } else {
                    entry.hasOptimal = true;
                }
            }
            
            // Test export/import
            bool linearExportOk = false;
            bool optimalExportOk = false;
            bool linearImportOk = false;
            bool optimalImportOk = false;
            
            // Test LINEAR if available
            if (entry.hasLinear) {
                VkSharedBaseObj<VkImageResource> testImage;
                VkResult res = createExportableImage(fmt, DRM_FORMAT_MOD_LINEAR, testImage);
                linearExportOk = (res == VK_SUCCESS);
                
                if (linearExportOk) {
                    int fd = -1;
                    res = exportDmaBufFd(testImage, &fd);
                    if (res == VK_SUCCESS && fd >= 0) {
                        uint64_t actualMod = DRM_FORMAT_MOD_LINEAR;
                        queryImageDrmModifier(testImage->GetImage(), &actualMod);
                        
                        VkMemoryRequirements memReqs;
                        m_vkDevCtx.GetImageMemoryRequirements(m_vkDevCtx.getDevice(), 
                                                              testImage->GetImage(), &memReqs);
                        
                        // Query actual plane layouts from exported image
                        uint32_t pc = fmt.planeCount > 0 ? fmt.planeCount : 1;
                        VkSubresourceLayout layouts[4] = {};
                        for (uint32_t p = 0; p < pc; p++) {
                            VkImageSubresource subres{};
                            subres.aspectMask = (pc > 1) ? (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
                            m_vkDevCtx.GetImageSubresourceLayout(m_vkDevCtx.getDevice(), testImage->GetImage(), &subres, &layouts[p]);
                        }
                        
                        VkSharedBaseObj<VkImageResource> importedImage;
                        res = importDmaBufImage(fmt, fd, memReqs.size, actualMod, 
                                               memReqs.memoryTypeBits, layouts, pc, importedImage);
                        linearImportOk = (res == VK_SUCCESS);
                    }
                }
            }
            
            // Test OPTIMAL (first non-linear modifier) if available
            if (entry.hasOptimal) {
                uint64_t optMod = DRM_FORMAT_MOD_INVALID;
                for (const auto& mod : modifiers) {
                    if (!mod.isLinear()) {
                        optMod = mod.modifier;
                        break;
                    }
                }
                
                if (optMod != DRM_FORMAT_MOD_INVALID) {
                    VkSharedBaseObj<VkImageResource> testImage;
                    VkResult res = createExportableImage(fmt, optMod, testImage);
                    optimalExportOk = (res == VK_SUCCESS);
                    
                    if (optimalExportOk) {
                        int fd = -1;
                        res = exportDmaBufFd(testImage, &fd);
                        if (res == VK_SUCCESS && fd >= 0) {
                            uint64_t actualMod = optMod;
                            queryImageDrmModifier(testImage->GetImage(), &actualMod);
                            
                            VkMemoryRequirements memReqs;
                            m_vkDevCtx.GetImageMemoryRequirements(m_vkDevCtx.getDevice(), 
                                                                  testImage->GetImage(), &memReqs);
                            
                            // Query actual plane layouts from exported image
                            uint32_t pc = fmt.planeCount > 0 ? fmt.planeCount : 1;
                            VkSubresourceLayout layouts[4] = {};
                            for (uint32_t p = 0; p < pc; p++) {
                                VkImageSubresource subres{};
                                subres.aspectMask = (pc > 1) ? (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
                                m_vkDevCtx.GetImageSubresourceLayout(m_vkDevCtx.getDevice(), testImage->GetImage(), &subres, &layouts[p]);
                            }
                            
                            VkSharedBaseObj<VkImageResource> importedImage;
                            res = importDmaBufImage(fmt, fd, memReqs.size, actualMod, 
                                                   memReqs.memoryTypeBits, layouts, pc, importedImage);
                            optimalImportOk = (res == VK_SUCCESS);
                        }
                    }
                }
            }
            
            entry.linearExportWorks = linearExportOk;
            entry.optimalExportWorks = optimalExportOk;
            entry.linearImportWorks = linearImportOk;
            entry.optimalImportWorks = optimalImportOk;
            
            // Determine overall status
            if ((entry.hasLinear && linearExportOk && linearImportOk) ||
                (entry.hasOptimal && optimalExportOk && optimalImportOk)) {
                if (entry.hasOptimal) {
                    entry.status = FormatSupportStatus::SUPPORTED;
                } else {
                    entry.status = FormatSupportStatus::LINEAR_ONLY;
                }
            } else if ((entry.hasLinear && linearExportOk) ||
                       (entry.hasOptimal && optimalExportOk)) {
                entry.status = FormatSupportStatus::IMPORT_FAIL;
                entry.notes = "Export works but import fails";
            } else {
                entry.status = FormatSupportStatus::EXPORT_FAIL;
                entry.notes = "Export fails";
            }
            
            // Override for video formats that fail
            if (entry.vulkanVideoSupport && 
                (entry.status == FormatSupportStatus::EXPORT_FAIL ||
                 entry.status == FormatSupportStatus::IMPORT_FAIL)) {
                entry.status = FormatSupportStatus::VIDEO_DRM_FAIL;
                entry.notes = "Vulkan Video format with DRM issues!";
            }
        }
        
        report.push_back(entry);
        
        if (m_config.verbose) {
            std::cout << "  " << fmt.name << ": " 
                      << formatSupportStatusToString(entry.status);
            if (entry.vulkanVideoSupport) {
                std::cout << " [VIDEO]";
            }
            std::cout << std::endl;
        }
    }
    
    return report;
}

//=============================================================================
// Print Report
//=============================================================================

void DrmFormatModTest::printReport(const std::vector<FormatSupportEntry>& report) const {
    std::cout << "\n================================================================================\n";
    std::cout << "                     DRM FORMAT MODIFIER SUPPORT REPORT\n";
    std::cout << "================================================================================\n\n";
    
    // Summary counts
    int supported = 0, notSupported = 0, videoDrmFail = 0, linearOnly = 0;
    int exportFail = 0, importFail = 0;
    int videoFormats = 0;
    
    for (const auto& e : report) {
        switch (e.status) {
            case FormatSupportStatus::SUPPORTED: supported++; break;
            case FormatSupportStatus::NOT_SUPPORTED: notSupported++; break;
            case FormatSupportStatus::VIDEO_DRM_FAIL: videoDrmFail++; break;
            case FormatSupportStatus::LINEAR_ONLY: linearOnly++; break;
            case FormatSupportStatus::EXPORT_FAIL: exportFail++; break;
            case FormatSupportStatus::IMPORT_FAIL: importFail++; break;
            default: break;
        }
        if (e.vulkanVideoSupport) videoFormats++;
    }
    
    std::cout << "SUMMARY:\n";
    std::cout << "--------\n";
    std::cout << "  Total formats tested:        " << report.size() << "\n";
    std::cout << "  Vulkan Video formats:        " << videoFormats << "\n";
    std::cout << "  Fully supported:             " << supported << "\n";
    std::cout << "  LINEAR only:                 " << linearOnly << "\n";
    std::cout << "  Not supported:               " << notSupported << "\n";
    std::cout << "  Export failures:             " << exportFail << "\n";
    std::cout << "  Import failures:             " << importFail << "\n";
    std::cout << "  VIDEO DRM FAILURES:          " << videoDrmFail << "\n\n";
    
    // Detailed table
    std::cout << std::left;
    std::cout << std::setw(45) << "FORMAT" 
              << std::setw(18) << "STATUS"
              << std::setw(8) << "LINEAR"
              << std::setw(8) << "TILED"
              << std::setw(6) << "VIDEO"
              << std::setw(5) << "MODS"
              << "NOTES\n";
    std::cout << std::string(100, '-') << "\n";
    
    for (const auto& e : report) {
        std::cout << std::setw(45) << e.format.name
                  << std::setw(18) << formatSupportStatusToString(e.status)
                  << std::setw(8) << (e.hasLinear ? "YES" : "NO")
                  << std::setw(8) << (e.hasOptimal ? "YES" : "NO")
                  << std::setw(6) << (e.vulkanVideoSupport ? "YES" : "-")
                  << std::setw(5) << e.modifierCount
                  << e.notes << "\n";
    }
    
    std::cout << std::string(100, '-') << "\n\n";
    
    // Highlight VIDEO DRM failures
    if (videoDrmFail > 0) {
        std::cout << "*** WARNING: VIDEO DRM FAILURES ***\n";
        std::cout << "The following Vulkan Video formats lack proper DRM modifier support:\n";
        for (const auto& e : report) {
            if (e.status == FormatSupportStatus::VIDEO_DRM_FAIL) {
                std::cout << "  - " << e.format.name << " (" << vkFormatToString(e.format.vkFormat) 
                          << "): " << e.notes << "\n";
            }
        }
        std::cout << "\n";
    }
    
    // Print verbose details if requested
    if (m_config.verbose) {
        std::cout << "\nDETAILED MODIFIER INFO:\n";
        std::cout << "=======================\n";
        
        for (const auto& e : report) {
            if (e.modifierCount > 0) {
                std::cout << "\n" << e.format.name << ":\n";
                auto modifiers = queryDrmModifiers(e.format.vkFormat);
                for (const auto& mod : modifiers) {
                    std::cout << "    " << mod.modifierToString()
                              << " planes=" << mod.planeCount
                              << " features=" << mod.featuresToString() << "\n";
                }
                
                std::cout << "    Export: LINEAR=" << (e.linearExportWorks ? "PASS" : "FAIL")
                          << " TILED=" << (e.optimalExportWorks ? "PASS" : "N/A") << "\n";
                std::cout << "    Import: LINEAR=" << (e.linearImportWorks ? "PASS" : "FAIL")
                          << " TILED=" << (e.optimalImportWorks ? "PASS" : "N/A") << "\n";
            }
        }
    }
}

//=============================================================================
// Save Report to File
//=============================================================================

void DrmFormatModTest::saveReportToFile(const std::vector<FormatSupportEntry>& report, 
                                         const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open report file: " << filename << std::endl;
        return;
    }
    
    file << "# DRM Format Modifier Support Report\n\n";
    
    // Get device name
    VkPhysicalDeviceProperties props;
    m_vkDevCtx.GetPhysicalDeviceProperties(m_vkDevCtx.getPhysicalDevice(), &props);
    file << "**Device:** " << props.deviceName << "\n";
    file << "**Driver Version:** " << VK_VERSION_MAJOR(props.driverVersion) << "."
         << VK_VERSION_MINOR(props.driverVersion) << "."
         << VK_VERSION_PATCH(props.driverVersion) << "\n\n";
    
    // Summary
    int supported = 0, notSupported = 0, videoDrmFail = 0, linearOnly = 0;
    int videoFormats = 0;
    
    for (const auto& e : report) {
        switch (e.status) {
            case FormatSupportStatus::SUPPORTED: supported++; break;
            case FormatSupportStatus::NOT_SUPPORTED: notSupported++; break;
            case FormatSupportStatus::VIDEO_DRM_FAIL: videoDrmFail++; break;
            case FormatSupportStatus::LINEAR_ONLY: linearOnly++; break;
            default: break;
        }
        if (e.vulkanVideoSupport) videoFormats++;
    }
    
    file << "## Summary\n\n";
    file << "| Metric | Count |\n";
    file << "|--------|-------|\n";
    file << "| Total formats | " << report.size() << " |\n";
    file << "| Vulkan Video formats | " << videoFormats << " |\n";
    file << "| Fully supported | " << supported << " |\n";
    file << "| LINEAR only | " << linearOnly << " |\n";
    file << "| Not supported | " << notSupported << " |\n";
    file << "| **VIDEO DRM FAILURES** | **" << videoDrmFail << "** |\n\n";
    
    // Detailed table
    file << "## Format Details\n\n";
    file << "| Format | Status | LINEAR | TILED | Video | Modifiers | Notes |\n";
    file << "|--------|--------|--------|-------|-------|-----------|-------|\n";
    
    for (const auto& e : report) {
        file << "| " << e.format.name
             << " | " << formatSupportStatusToString(e.status)
             << " | " << (e.hasLinear ? "Yes" : "No")
             << " | " << (e.hasOptimal ? "Yes" : "No")
             << " | " << (e.vulkanVideoSupport ? "**Yes**" : "-")
             << " | " << e.modifierCount
             << " | " << e.notes << " |\n";
    }
    
    // VIDEO DRM failures section
    if (videoDrmFail > 0) {
        file << "\n## Critical: VIDEO DRM Failures\n\n";
        file << "These Vulkan Video formats lack proper DRM modifier support:\n\n";
        for (const auto& e : report) {
            if (e.status == FormatSupportStatus::VIDEO_DRM_FAIL) {
                file << "- **" << e.format.name << "** (`" << vkFormatToString(e.format.vkFormat) 
                     << "`): " << e.notes << "\n";
            }
        }
    }
    
    file.close();
    std::cout << "[INFO] Report saved to: " << filename << std::endl;
}

//=============================================================================
// Print Test Summary
//=============================================================================

void printTestSummary(const std::vector<TestResult>& results, bool verbose) {
    int passed = 0, failed = 0, skipped = 0;
    
    for (const auto& r : results) {
        switch (r.status) {
            case TestStatus::PASS: passed++; break;
            case TestStatus::FAIL:
            case TestStatus::ERROR: failed++; break;
            case TestStatus::SKIP: skipped++; break;
        }
    }
    
    std::cout << "\n=== Test Summary ===" << std::endl;
    std::cout << "Total: " << results.size() 
              << ", Passed: " << passed 
              << ", Failed: " << failed 
              << ", Skipped: " << skipped << std::endl;
    
    // In verbose mode, print all results
    if (verbose) {
        std::cout << "\nAll test results:" << std::endl;
        for (const auto& r : results) {
            std::cout << "  [" << testStatusToString(r.status) << "] " 
                      << r.testName << ": " << r.message;
            if (r.durationMs > 0) {
                std::cout << " (" << std::fixed << std::setprecision(2) 
                          << r.durationMs << "ms)";
            }
            std::cout << std::endl;
        }
    }
    
    // Always print failures
    if (failed > 0) {
        std::cout << "\nFailed tests:" << std::endl;
        for (const auto& r : results) {
            if (r.status == TestStatus::FAIL || r.status == TestStatus::ERROR) {
                std::cout << "  " << r.testName << ": " << r.message << std::endl;
            }
        }
    }
}

} // namespace drm_format_mod_test
