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

// Tightly-packed per-plane geometry for a test format, in exactly the byte layout the
// CPU reference model in ColorConversion.cpp produces: plane 0 first, then the chroma
// plane(s) in order, no row padding anywhere.
//
// This is the SINGLE description of that layout. calculateImageSize() is its sum and the
// staging-copy regions are built from it, so a copy and a size cannot disagree --
// which matters because a staging copy that lands the chroma plane at the wrong offset
// still returns VK_SUCCESS and still produces a full buffer of plausible bytes.
//
// Everything is derived from YcbcrVkFormatInfo(), the same table the filter's own
// CalculateBufferImageCopyRegions() reads, rather than from a per-format switch.
static uint32_t calculatePlaneGeometry(TestFormat format, uint32_t width, uint32_t height,
                                       PlaneBufferGeometry geometry[TransferResource::kMaxPlanes]) {
    const VkFormat vkFormat = toVkFormat(format);
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(vkFormat);

    if ((mpInfo == nullptr) || (mpInfo->planesLayout.numberOfExtraPlanes == 0)) {
        // Single-plane: RGBA8/BGRA8, and the packed 4:4:4 aliases (Y410 rides
        // A2B10G10R10_UNORM_PACK32, which YcbcrVkFormatInfo() does not describe because
        // its table is densely indexed over the Y'CbCr enum range). Four bytes per
        // whole pixel in every case here.
        geometry[0] = {};
        geometry[0].width         = width;
        geometry[0].height        = height;
        geometry[0].bytesPerPixel = 4;
        geometry[0].rowPitch      = (VkDeviceSize)width * 4;
        geometry[0].size          = geometry[0].rowPitch * height;
        return 1;
    }

    const uint32_t numPlanes       = mpInfo->planesLayout.numberOfExtraPlanes + 1;
    const uint32_t bytesPerChannel = (GetBitsPerChannel(mpInfo->planesLayout) > 8) ? 2 : 1;
    const uint32_t subX            = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledX;
    const uint32_t subY            = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledY;

    VkDeviceSize offset = 0;
    for (uint32_t plane = 0; (plane < numPlanes) && (plane < TransferResource::kMaxPlanes); plane++) {
        const bool isLuma = (plane == 0);
        // A 2-plane format interleaves Cb and Cr in plane 1, so that plane carries two
        // channels per texel; a 3-plane format carries one in each.
        const uint32_t channelsPerTexel = (isLuma || (numPlanes == 3)) ? 1 : 2;

        geometry[plane] = {};
        geometry[plane].offset        = offset;
        geometry[plane].width         = isLuma ? width  : (width  / subX);
        geometry[plane].height        = isLuma ? height : (height / subY);
        geometry[plane].bytesPerPixel = channelsPerTexel * bytesPerChannel;
        geometry[plane].rowPitch      = (VkDeviceSize)geometry[plane].width *
                                        geometry[plane].bytesPerPixel;
        geometry[plane].size          = geometry[plane].rowPitch * geometry[plane].height;
        offset += geometry[plane].size;
    }
    return numPlanes;
}

// NOTE on 10/12-bit sample alignment: VK_FORMAT_G10X6_B10X6R10X6_2PLANE_* carries its 10
// bits in the TOP of each 16-bit word (the "X6" is six unused LOW bits), and likewise X4
// at 12-bit. rgbToYCbCr16() already applies that shift before it returns, so callers must
// NOT shift again -- doing so silently overflows the uint16 and lands the sample at an
// unrelated value (60288 << 6 wraps to 57344), which reads as a plausible-looking
// mid-range number rather than as an error.

// Append 16-bit samples to a byte vector in the buffer's own (little-endian) order.
static void appendSamples16(std::vector<uint8_t>& bytes, const std::vector<uint16_t>& samples) {
    const size_t offset = bytes.size();
    bytes.resize(offset + samples.size() * sizeof(uint16_t));
    memcpy(bytes.data() + offset, samples.data(), samples.size() * sizeof(uint16_t));
}

// True for the formats whose samples are 16-bit words rather than bytes. Comparing those
// byte-wise against a 255 peak reports a number, but not a meaningful one.
static bool formatIs16BitSamples(TestFormat format) {
    return (format == TestFormat::P010) ||
           (format == TestFormat::P012) ||
           (format == TestFormat::P210);
}

static size_t calculateImageSize(TestFormat format, uint32_t width, uint32_t height);

// =============================================================================
// Canonical-frame model for Y'CbCr -> Y'CbCr reference conversion
// =============================================================================
//
// A frame decoded into canonical form: Y, Cb and Cr at FULL resolution, normalized to
// [0,1]. Every cross-format reference goes through this, so N formats need N unpackers
// and N packers instead of N^2 pairwise conversions -- and, more to the point, adding a
// format cannot leave a pair silently unmodelled.
//
// Resampling policy deliberately mirrors the shader: chroma is REPLICATED from its
// subsampled position on the way in (GenReadYCbCrBlock reads the subsampled position),
// and BOX-AVERAGED over the block on the way out (the same generator averages when the
// output is more subsampled than the block). A reference that resampled differently would
// disagree with a correct filter, which is just a slower way of being wrong.
struct CanonicalFrame {
    std::vector<float> plane[3];   // 0 = Y, 1 = Cb, 2 = Cr; each width*height
    uint32_t width{0};
    uint32_t height{0};
};

static float readCanonicalSample(const uint8_t* base, const PlaneBufferGeometry& geom,
                                 uint32_t x, uint32_t y, uint32_t channel, bool is16Bit) {
    const size_t bytesPerChannel = is16Bit ? 2u : 1u;
    const size_t offset = (size_t)geom.offset
                        + (size_t)y * (size_t)geom.rowPitch
                        + (size_t)x * (size_t)geom.bytesPerPixel
                        + (size_t)channel * bytesPerChannel;
    if (is16Bit) {
        uint16_t value = 0;
        memcpy(&value, base + offset, sizeof(value));
        return (float)value / 65535.0f;
    }
    return (float)base[offset] / 255.0f;
}

static void writeCanonicalSample(uint8_t* base, const PlaneBufferGeometry& geom,
                                 uint32_t x, uint32_t y, uint32_t channel, bool is16Bit,
                                 float normalized) {
    const size_t bytesPerChannel = is16Bit ? 2u : 1u;
    const size_t offset = (size_t)geom.offset
                        + (size_t)y * (size_t)geom.rowPitch
                        + (size_t)x * (size_t)geom.bytesPerPixel
                        + (size_t)channel * bytesPerChannel;

    const float clamped = (normalized < 0.0f) ? 0.0f : ((normalized > 1.0f) ? 1.0f : normalized);
    if (is16Bit) {
        const uint16_t value = (uint16_t)(clamped * 65535.0f + 0.5f);
        memcpy(base + offset, &value, sizeof(value));
    } else {
        base[offset] = (uint8_t)(clamped * 255.0f + 0.5f);
    }
}

static bool unpackToCanonical(const std::vector<uint8_t>& bytes, TestFormat format,
                              uint32_t width, uint32_t height, CanonicalFrame& out) {
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(toVkFormat(format));
    if ((mpInfo == nullptr) || (mpInfo->planesLayout.numberOfExtraPlanes == 0)) {
        return false;   // packed / RGBA: not modelled here
    }
    if (bytes.size() < calculateImageSize(format, width, height)) {
        return false;
    }

    PlaneBufferGeometry geom[TransferResource::kMaxPlanes];
    const uint32_t numPlanes = calculatePlaneGeometry(format, width, height, geom);
    const bool is16Bit = formatIs16BitSamples(format);
    const uint32_t subX = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledX;
    const uint32_t subY = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledY;

    out.width  = width;
    out.height = height;
    for (uint32_t p = 0; p < 3; p++) {
        out.plane[p].assign((size_t)width * height, 0.0f);
    }

    const uint8_t* base = bytes.data();
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const size_t idx = (size_t)y * width + x;
            out.plane[0][idx] = readCanonicalSample(base, geom[0], x, y, 0, is16Bit);

            const uint32_t cx = x / subX;
            const uint32_t cy = y / subY;
            if (numPlanes == 2) {
                // Cb and Cr interleaved in plane 1.
                out.plane[1][idx] = readCanonicalSample(base, geom[1], cx, cy, 0, is16Bit);
                out.plane[2][idx] = readCanonicalSample(base, geom[1], cx, cy, 1, is16Bit);
            } else {
                out.plane[1][idx] = readCanonicalSample(base, geom[1], cx, cy, 0, is16Bit);
                out.plane[2][idx] = readCanonicalSample(base, geom[2], cx, cy, 0, is16Bit);
            }
        }
    }
    return true;
}

static bool packFromCanonical(const CanonicalFrame& in, TestFormat format,
                              uint32_t width, uint32_t height, std::vector<uint8_t>& bytes) {
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(toVkFormat(format));
    if ((mpInfo == nullptr) || (mpInfo->planesLayout.numberOfExtraPlanes == 0)) {
        return false;
    }
    if ((in.width != width) || (in.height != height)) {
        return false;   // this model does not rescale
    }

    PlaneBufferGeometry geom[TransferResource::kMaxPlanes];
    const uint32_t numPlanes = calculatePlaneGeometry(format, width, height, geom);
    const bool is16Bit = formatIs16BitSamples(format);
    const uint32_t subX = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledX;
    const uint32_t subY = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledY;

    bytes.assign(calculateImageSize(format, width, height), 0);
    uint8_t* base = bytes.data();

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            writeCanonicalSample(base, geom[0], x, y, 0, is16Bit,
                                 in.plane[0][(size_t)y * width + x]);
        }
    }

    const float blockScale = 1.0f / (float)(subX * subY);
    for (uint32_t cy = 0; cy < geom[1].height; cy++) {
        for (uint32_t cx = 0; cx < geom[1].width; cx++) {
            float cbSum = 0.0f;
            float crSum = 0.0f;
            for (uint32_t dy = 0; dy < subY; dy++) {
                for (uint32_t dx = 0; dx < subX; dx++) {
                    const size_t idx = (size_t)(cy * subY + dy) * width + (cx * subX + dx);
                    cbSum += in.plane[1][idx];
                    crSum += in.plane[2][idx];
                }
            }
            const float cb = cbSum * blockScale;
            const float cr = crSum * blockScale;

            if (numPlanes == 2) {
                writeCanonicalSample(base, geom[1], cx, cy, 0, is16Bit, cb);
                writeCanonicalSample(base, geom[1], cx, cy, 1, is16Bit, cr);
            } else {
                writeCanonicalSample(base, geom[1], cx, cy, 0, is16Bit, cb);
                writeCanonicalSample(base, geom[2], cx, cy, 0, is16Bit, cr);
            }
        }
    }
    return true;
}

// The usage and create flags every test image is made with. Shared between the support
// query and vkCreateImage so the two cannot disagree about what is being asked for --
// a support check that asks a different question than the creation is worse than none.
static constexpr VkImageUsageFlags kTestImageUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static VkImageCreateFlags testImageCreateFlags(TestFormat format) {
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(toVkFormat(format));
    // Multi-planar formats need MUTABLE + EXTENDED usage for the per-plane storage views.
    return (mpInfo && (mpInfo->planesLayout.numberOfExtraPlanes > 0))
        ? (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
        : 0;
}

static size_t calculateImageSize(TestFormat format, uint32_t width, uint32_t height) {
    if (toVkFormat(format) == VK_FORMAT_UNDEFINED) {
        return 0;
    }

    PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
    const uint32_t numPlanes = calculatePlaneGeometry(format, width, height, geometry);

    size_t total = 0;
    for (uint32_t plane = 0; plane < numPlanes; plane++) {
        total += (size_t)geometry[plane].size;
    }
    return total;
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

VkResult FilterTestApp::init(bool verbose, const char* deviceUuidStr) {
    
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
    // Resolve the GPU by UUID when one is given. On a multi-GPU box the auto-selection
    // picks whatever enumerates first -- here, an Intel iGPU -- so a run can silently
    // report on a device other than the one under test. Index-based selection is not an
    // option: nvidia-smi, the Vulkan loader and the profiling tools each enumerate in a
    // different order.
    vk::DeviceUuidUtils deviceUuid;  // Empty UUID = auto-select
    if ((deviceUuidStr != nullptr) && (deviceUuidStr[0] != '\0')) {
        if (deviceUuid.StringToUUID(deviceUuidStr) == 0) {
            std::cerr << "[FilterTestApp] Could not parse --deviceUuid '" << deviceUuidStr
                      << "' (expected 36 characters, 32 hex digits and 4 hyphens)" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

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
    
    // A subsampled format cannot describe an odd dimension: Vulkan requires extent.width
    // (and, for 4:2:0, extent.height) to be a multiple of the subsampling
    // (VUID-VkImageCreateInfo-format-04712/04713). Asking anyway is not a filter test, it
    // is undefined behaviour that happens to return an image: the driver rounds the
    // chroma plane up, the reference model floor-divides it, and the mismatch reads as a
    // filter defect. Reject the extent instead of scoring the result.
    auto checkSubsampledExtent = [](const TestIOSlot& slot, const char* which,
                                    TestResult& res) -> bool {
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(toVkFormat(slot.format));
        if ((mpInfo == nullptr) || (mpInfo->planesLayout.numberOfExtraPlanes == 0)) {
            return true;
        }
        const uint32_t subX = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledX;
        const uint32_t subY = 1u << mpInfo->planesLayout.secondaryPlaneSubsampledY;
        if (((slot.width % subX) != 0) || ((slot.height % subY) != 0)) {
            res.errorMessage = std::string(which) + " extent " + std::to_string(slot.width) +
                               "x" + std::to_string(slot.height) + " is not a multiple of the " +
                               std::to_string(subX) + "x" + std::to_string(subY) +
                               " chroma subsampling of " + testFormatName(slot.format);
            res.passed = false;
            res.unvalidated = true;
            return false;
        }
        return true;
    };
    for (const auto& input : config.inputs) {
        if (!checkSubsampledExtent(input, "Input", result)) {
            return result;
        }
    }
    for (const auto& output : config.outputs) {
        if (!checkSubsampledExtent(output, "Output", result)) {
            return result;
        }
    }

    // Check format support. A device that does not implement a format is not a test
    // failure, but it is also not coverage -- report it as unvalidated so it can never
    // be mistaken for a passing case.
    for (const auto& input : config.inputs) {
        if (!isFormatSupported(input.format, input.resourceType, input.tiling)) {
            result.errorMessage = "Input format not supported by this device: " +
                                  std::string(testFormatName(input.format));
            result.passed = false;
            result.unvalidated = true;
            return result;
        }
    }
    for (const auto& output : config.outputs) {
        if (!isFormatSupported(output.format, output.resourceType, output.tiling)) {
            result.errorMessage = "Output format not supported by this device: " +
                                  std::string(testFormatName(output.format));
            result.passed = false;
            result.unvalidated = true;
            return result;
        }
    }
    
    // Create input resources
    std::vector<VkSharedBaseObj<VkImageResource>> inputImages;
    std::vector<VkSharedBaseObj<VkImageResourceView>> inputImageViews;
    std::vector<VkSharedBaseObj<VkBufferResource>> inputBuffers;
    
    // Bytes actually written into input slot 0, kept so the CPU reference model can be
    // computed from the same data the shader reads.
    std::vector<uint8_t> firstInputPattern;

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
            vkResult = generateTestPattern(inputSlot, image, buffer,
                                           (&inputSlot == &config.inputs[0]) ? &firstInputPattern
                                                                             : nullptr);
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
    auto* yuvFilter = static_cast<VulkanFilterYuvCompute*>(filter.get());
    
    // Set up resource info
    VkVideoPictureResourceInfoKHR inputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    inputResourceInfo.codedExtent = {config.inputs[0].width, config.inputs[0].height};
    inputResourceInfo.baseArrayLayer = 0;
    
    VkVideoPictureResourceInfoKHR outputResourceInfo{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    outputResourceInfo.codedExtent = {config.outputs[0].width, config.outputs[0].height};
    outputResourceInfo.baseArrayLayer = 0;
    
    // Staging buffers for optimal-tiled slots. Declared here because they must stay
    // alive until the submission that references them has completed.
    VkSharedBaseObj<VkBufferResource> inputStagingBuffer;
    VkSharedBaseObj<VkBufferResource> outputStagingBuffer;

    // Record based on resource types
    if (config.inputs[0].resourceType == ResourceType::Image &&
        config.outputs[0].resourceType == ResourceType::Image) {

        const TestIOSlot& inSlot  = config.inputs[0];
        const TestIOSlot& outSlot = config.outputs[0];

        // Both tilings go through staging, not just optimal.
        //
        // A linear image is host-mappable, but its plane offsets and row pitches are the
        // driver's business: NVIDIA pads them, other implementations pack them tightly.
        // A memcpy straight into the mapping therefore matches on luma and reads every
        // chroma sample from the wrong offset on exactly the drivers that pad, which
        // makes the result a property of the ICD rather than of the filter. Going through
        // vkCmdCopyBufferToImage / vkCmdCopyImageToBuffer makes the driver reconcile its
        // own layout with our tightly-packed buffer geometry, which is exactly the
        // question we do not want to answer ourselves.
        const bool needsInputUpload    = !firstInputPattern.empty();
        const bool needsOutputReadback = outSlot.validateOutput;

        if (needsInputUpload || needsOutputReadback) {
            // Use the filter's own pre/post-transfer machinery rather than hand-rolling
            // the copies: it already derives one VkBufferImageCopy per plane with the
            // right aspect mask and subsampled extent, and puts the transfers, the
            // barriers and the dispatch in a single submission.
            FilterExecutionDesc execDesc{};
            execDesc.numInputs  = 1;
            execDesc.numOutputs = 1;

            execDesc.inputs[0].primary =
                TransferResource::fromImageResource(*inputImages[0], VK_IMAGE_LAYOUT_UNDEFINED);
            execDesc.inputs[0].primaryView = inputImageViews[0].get();

            execDesc.outputs[0].primary =
                TransferResource::fromImageResource(*outputImages[0], VK_IMAGE_LAYOUT_UNDEFINED);
            execDesc.outputs[0].primaryView = outputImageViews[0].get();

            if (needsInputUpload) {
                vkResult = createStagingBuffer(firstInputPattern.size(), inputStagingBuffer,
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                if (vkResult != VK_SUCCESS) {
                    result.errorMessage = "Failed to create input staging buffer";
                    result.passed = false;
                    m_vkDevCtx.EndCommandBuffer(cmdBuffer);
                    m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
                    return result;
                }

                VkDeviceSize maxSize = 0;
                uint8_t* stagingData = inputStagingBuffer->GetDataPtr(0, maxSize);
                if ((stagingData == nullptr) || (maxSize < firstInputPattern.size())) {
                    result.errorMessage = "Input staging buffer is not host-mappable at the required size";
                    result.passed = false;
                    m_vkDevCtx.EndCommandBuffer(cmdBuffer);
                    m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
                    return result;
                }
                memcpy(stagingData, firstInputPattern.data(), firstInputPattern.size());

                PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
                const uint32_t numPlanes = calculatePlaneGeometry(inSlot.format,
                                                                  inSlot.width, inSlot.height,
                                                                  geometry);
                execDesc.inputs[0].preTransferSource =
                    TransferResource::fromBuffer(inputStagingBuffer->GetBuffer(),
                                                 firstInputPattern.size(),
                                                 numPlanes, geometry);
                execDesc.inputs[0].preTransferOp = TransferOpType::COPY_BUFFER_TO_IMAGE;
            }

            if (needsOutputReadback) {
                const size_t outputSize = calculateImageSize(outSlot.format,
                                                             outSlot.width, outSlot.height);
                vkResult = createStagingBuffer(outputSize, outputStagingBuffer,
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                if (vkResult != VK_SUCCESS) {
                    result.errorMessage = "Failed to create output staging buffer";
                    result.passed = false;
                    m_vkDevCtx.EndCommandBuffer(cmdBuffer);
                    m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
                    return result;
                }

                PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
                const uint32_t numPlanes = calculatePlaneGeometry(outSlot.format,
                                                                  outSlot.width, outSlot.height,
                                                                  geometry);
                execDesc.outputs[0].postTransferDest =
                    TransferResource::fromBuffer(outputStagingBuffer->GetBuffer(),
                                                 outputSize, numPlanes, geometry);
                execDesc.outputs[0].postTransferOp = TransferOpType::COPY_IMAGE_TO_BUFFER;
            }

            vkResult = yuvFilter->RecordCommandBuffer(cmdBuffer, 0 /* bufferIdx */, execDesc);
        } else {
            vkResult = yuvFilter->RecordCommandBuffer(
                cmdBuffer,
                0,  // bufferIdx
                inputImageViews[0].get(),
                &inputResourceInfo,
                outputImageViews[0].get(),
                &outputResourceInfo
            );
        }
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
    
    // Validate output. A buffer output is read from its own mapping; an image output of
    // either tiling was copied into outputStagingBuffer by the post-transfer above, which
    // is host-visible and holds the planes tightly packed in reference order.
    const TestIOSlot& outputSlot = config.outputs[0];

    if (outputSlot.validateOutput) {
        if (outputSlot.resourceType == ResourceType::Buffer || outputStagingBuffer) {
            // Compare against a CPU reference computed from the bytes that were actually
            // uploaded. An empty reference makes validateOutput fall through to "did we
            // get any bytes back", which a filter that writes garbage -- or writes only
            // part of the frame -- passes just as happily as a correct one.
            std::vector<uint8_t> referenceData =
                generateReferenceOutput(config, firstInputPattern);

            if (referenceData.empty() && !firstInputPattern.empty()) {
                // No CPU model for this conversion yet. Say so instead of reporting a
                // pass: an unvalidated case must not look like a validated one.
                result.passed = false;
                result.unvalidated = true;
                result.errorMessage =
                    "No CPU reference model for this format pair - cannot validate "
                    "(add one to generateReferenceOutput, or clear validateOutput)";
            } else {
                // Prefer the readback staging buffer when there is one: for an optimal
                // image it is the only host-readable copy of the result.
                VkSharedBaseObj<VkBufferResource>& readbackBuffer =
                    outputStagingBuffer ? outputStagingBuffer : outputBuffers[0];

                TestResult valResult = validateOutput(config, outputSlot,
                                                     outputImages[0], readbackBuffer,
                                                     referenceData);
                result.passed = valResult.passed;
                result.psnrY = valResult.psnrY;
                result.psnrCb = valResult.psnrCb;
                result.psnrCr = valResult.psnrCr;
                if (!valResult.errorMessage.empty()) {
                    result.errorMessage = valResult.errorMessage;
                }
            }
        } else {
            // Reached only when an optimal output asked to be validated but no readback
            // staging buffer was set up. Report the gap instead of a pass: an unvalidated
            // case must never be counted as coverage.
            result.passed = false;
            result.unvalidated = true;
            result.errorMessage =
                "Optimal-tiled output was not read back - output not validated";
        }
    } else {
        // No validation requested - just check execution succeeded
        result.passed = true;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    std::cout << "[Test] " << config.name << ": " 
              << (result.passed ? "PASSED" : (result.unvalidated ? "UNVALIDATED" : "FAILED"))
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
    int unvalidated = 0;
    
    for (const auto& result : results) {
        if (result.passed) {
            passed++;
            std::cout << "[PASS] " << result.testName << std::endl;
        } else if (result.unvalidated) {
            unvalidated++;
            std::cout << "[UNVALIDATED] " << result.testName << ": " << result.errorMessage << std::endl;
        } else {
            failed++;
            std::cout << "[FAIL] " << result.testName << ": " << result.errorMessage << std::endl;
        }
    }
    
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Total: " << results.size() << ", Passed: " << passed
              << ", Failed: " << failed
              << ", Unvalidated: " << unvalidated << std::endl;
    if (passed == 0) {
        // A run with nothing validated is not a green run, whatever the failure count
        // says: a suite that checks no pixels at all reports the same totals as one that
        // checks them and finds them right. Call it out.
        std::cout << "WARNING: no case in this run validated its output pixels."
                  << std::endl;
    }
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
    }

    // Single-plane formats: check the format itself, then fall through to the image-level
    // query below.
    if (!mpInfo || mpInfo->planesLayout.numberOfExtraPlanes == 0) {
        VkFormatProperties formatProps;
        m_vkDevCtx.GetPhysicalDeviceFormatProperties(m_vkDevCtx.getPhysicalDevice(), vkFormat, &formatProps);

        const VkFormatFeatureFlags available = (tiling == TilingMode::Optimal)
            ? formatProps.optimalTilingFeatures
            : formatProps.linearTilingFeatures;
        if ((available & requiredFeatures) == 0) {
            return false;
        }
    }

    // Per-plane format features are necessary but not sufficient: the driver can still
    // reject (or mishandle) the multi-planar image itself. Ask about the exact image
    // this test would create.
    //
    // This matters more than a tidiness argument. vkCreateImage on a format the driver
    // does not really support is not a clean VK_ERROR_FORMAT_NOT_SUPPORTED in practice:
    // Mesa's Intel ICD divides by zero inside vkCreateImage and takes the process down
    // with SIGFPE, so one unsupported format ends the whole run rather than one case.
    VkImageFormatProperties imageFormatProps{};
    const VkResult imageFormatResult = m_vkDevCtx.GetPhysicalDeviceImageFormatProperties(
        m_vkDevCtx.getPhysicalDevice(),
        vkFormat,
        VK_IMAGE_TYPE_2D,
        (tiling == TilingMode::Linear) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
        kTestImageUsage,
        testImageCreateFlags(format),
        &imageFormatProps);

    return (imageFormatResult == VK_SUCCESS);
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
        imageInfo.usage = kTestImageUsage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Multi-planar formats get MUTABLE + EXTENDED usage; same source as the query.
        imageInfo.flags |= testImageCreateFlags(slot.format);


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
                                           VkSharedBaseObj<VkBufferResource>& outBuffer,
                                           VkBufferUsageFlags usage) {
    return VkBufferResource::Create(
        &m_vkDevCtx,
        usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        size,
        outBuffer
    );
}

VkResult FilterTestApp::generateTestPattern(const TestIOSlot& slot,
                                           VkSharedBaseObj<VkImageResource>& image,
                                           VkSharedBaseObj<VkBufferResource>& buffer,
                                           std::vector<uint8_t>* pOutPatternData) {
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
            if (slot.format == TestFormat::P210) {
                convertRGBAtoP210(rgbaData.data(), slot.width, slot.height,
                                  ColorPrimaries::BT709, ColorRange::Full,
                                  yPlane16, uvPlane16);
            } else if (slot.format == TestFormat::P012) {
                convertRGBAtoP012(rgbaData.data(), slot.width, slot.height,
                                  ColorPrimaries::BT709, ColorRange::Full,
                                  yPlane16, uvPlane16);
            } else {
                convertRGBAtoP010(rgbaData.data(), slot.width, slot.height,
                                  ColorPrimaries::BT709, ColorRange::Full,
                                  yPlane16, uvPlane16);
            }

            patternData.clear();
            appendSamples16(patternData, yPlane16);
            appendSamples16(patternData, uvPlane16);
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
    
    // Optimal-tiled images cannot be written from the host at all. Their upload is a
    // staging buffer plus a vkCmdCopyBufferToImage, which runTest() sets up as the
    // filter's pre-transfer -- the staging buffer has to outlive this function, and the
    // copy has to be in the same submission as the compute dispatch. Neither is possible
    // from here, which is why this function only produces the bytes: a staging buffer
    // filled and dropped on return feeds the shader uninitialised memory, and the case
    // still reports success.

    // Note: linear images are NOT written through their host mapping here. See the
    // staging path in runTest() -- a direct memcpy assumes the planes are tightly packed,
    // which is not true on every driver.

    // Hand back exactly what was written, so the reference model is computed from the
    // same bytes the shader will read rather than from a regenerated pattern.
    if (pOutPatternData != nullptr) {
        *pOutPatternData = patternData;
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
    }
    // No direct read of a linear image's mapping: its planes are not necessarily tightly
    // packed. Everything arrives through the readback staging buffer instead.
    (void)outputImage;
    
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
                const size_t compareBytes = std::min(actualData.size(), referenceData.size());

                // 10/12-bit formats store one sample per 16-bit word. Comparing those a
                // byte at a time against a 255 peak produces a number that moves with the
                // data but means nothing: the high byte of every sample is compared on the
                // same footing as its low byte.
                if (getenv("VKFT_DIFF")) {
                    PlaneBufferGeometry g[TransferResource::kMaxPlanes];
                    const uint32_t np = calculatePlaneGeometry(outputSlot.format,
                                                               outputSlot.width, outputSlot.height, g);
                    for (uint32_t pl = 0; pl < np; pl++) {
                        size_t nDiff = 0, firstDiff = 0; double maxErr = 0;
                        const size_t begin = (size_t)g[pl].offset;
                        const size_t end = std::min(begin + (size_t)g[pl].size, compareBytes);
                        for (size_t i = begin; i < end; i++) {
                            const double d = std::abs((double)actualData[i] - (double)referenceData[i]);
                            if (d > 1.5) { if (!nDiff) firstDiff = i - begin; nDiff++; if (d > maxErr) maxErr = d; }
                        }
                        fprintf(stderr, "[diff] plane%u %ux%u pitch=%llu: %zu/%zu bytes differ, "
                                        "first at +%zu (row %zu col %zu), maxErr=%.0f\n",
                                pl, g[pl].width, g[pl].height, (unsigned long long)g[pl].rowPitch,
                                nDiff, end - begin, firstDiff,
                                g[pl].rowPitch ? firstDiff / (size_t)g[pl].rowPitch : 0,
                                g[pl].rowPitch ? firstDiff % (size_t)g[pl].rowPitch : 0, maxErr);
                    }
                }
                const double psnr = formatIs16BitSamples(outputSlot.format)
                    ? calculatePSNR16(actualData.data(), referenceData.data(), compareBytes)
                    : calculatePSNR(actualData.data(), referenceData.data(), compareBytes);

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
                    
                    case TestFormat::P010:
                    case TestFormat::P012:
                    case TestFormat::P210: {
                        std::vector<uint16_t> yPlane16, uvPlane16;
                        if (output.format == TestFormat::P210) {
                            convertRGBAtoP210(inputData.data(), input.width, input.height,
                                              primaries, range, yPlane16, uvPlane16);
                        } else if (output.format == TestFormat::P012) {
                            convertRGBAtoP012(inputData.data(), input.width, input.height,
                                              primaries, range, yPlane16, uvPlane16);
                        } else {
                            convertRGBAtoP010(inputData.data(), input.width, input.height,
                                              primaries, range, yPlane16, uvPlane16);
                        }

                        referenceData.clear();
                        appendSamples16(referenceData, yPlane16);
                        appendSamples16(referenceData, uvPlane16);
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
            if ((input.format == output.format) &&
                (input.width == output.width) && (input.height == output.height)) {
                // Same format in and out: the output really is the input, byte for byte.
                referenceData = inputData;
                break;
            }

            // Different format: the output bytes are legitimately NOT the input bytes --
            // a different plane layout, a different chroma subsampling, or a different
            // bit depth. Returning inputData here is not a weak check, it is a wrong
            // one: it asserts that an I420 frame equals the NV12 frame it came from,
            // which scores an exact luma plane against a chroma plane that cannot match
            // and so reads like a filter defect that is not there.
            //
            // It can also pass for the wrong reason: the comparison truncates to the
            // shorter buffer, so for NV12->NV16 the top half of the taller 4:2:2 chroma
            // plane matches the input and nothing below it is looked at.
            CanonicalFrame frame;
            if (unpackToCanonical(inputData, input.format, input.width, input.height, frame) &&
                packFromCanonical(frame, output.format, output.width, output.height, referenceData)) {
                break;
            }

            // No model for this pair (packed formats, or a resize). Leave the reference
            // empty so the case reports as unvalidated rather than as a pass.
            referenceData.clear();
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

double FilterTestApp::calculatePSNR16(const uint8_t* data1, const uint8_t* data2, size_t sizeBytes) {
    const size_t numSamples = sizeBytes / sizeof(uint16_t);
    if (numSamples == 0) {
        return 0.0;
    }

    // Read through memcpy rather than a uint16_t* cast: the staging buffer mapping has no
    // alignment guarantee beyond the allocation itself.
    double mse = 0.0;
    for (size_t i = 0; i < numSamples; i++) {
        uint16_t a = 0;
        uint16_t b = 0;
        memcpy(&a, data1 + i * sizeof(uint16_t), sizeof(uint16_t));
        memcpy(&b, data2 + i * sizeof(uint16_t), sizeof(uint16_t));
        const double diff = (double)a - (double)b;
        mse += diff * diff;
    }
    mse /= (double)numSamples;

    if (mse == 0.0) {
        return 100.0;
    }

    // Peak is the 16-bit container, because that is what the samples are stored in
    // after MSB alignment.
    const double maxVal = 65535.0;
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
