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

#include <cstdio>
#include <cstdlib>
#include <string>
#include "FilterTestApp.h"
#include "TestCases.h"
#include "ColorConversion.h"

#include <iostream>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

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
        case TestFormat::P212:   return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16;
        case TestFormat::YUV444: return VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
        case TestFormat::Y410:   return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // Packed AVYU 4:4:4
        case TestFormat::R8:     return VK_FORMAT_R8_UNORM;
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
        case TestFormat::P212:   return "P212 (12-bit 4:2:2)";
        case TestFormat::YUV444: return "YUV444 (8-bit 4:4:4)";
        case TestFormat::Y410:   return "Y410 (10-bit 4:4:4 packed)";
        case TestFormat::R8:     return "R8 (8-bit single-plane)";
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
           (format == TestFormat::P210) ||
           (format == TestFormat::P212);
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
    if (format == TestFormat::Y410) {
        // One little-endian 32-bit word per pixel: Cb in bits 9:0, Y in 19:10, Cr in 29:20, each
        // normalized as the A2B10G10R10 UNORM view the filter reads it through.
        if (bytes.size() < (size_t)width * height * 4) {
            return false;
        }
        out.width  = width;
        out.height = height;
        for (uint32_t p = 0; p < 3; p++) {
            out.plane[p].assign((size_t)width * height, 0.0f);
        }
        for (size_t i = 0; i < (size_t)width * height; i++) {
            const uint32_t word = (uint32_t)bytes[i * 4] | ((uint32_t)bytes[i * 4 + 1] << 8) |
                                  ((uint32_t)bytes[i * 4 + 2] << 16) | ((uint32_t)bytes[i * 4 + 3] << 24);
            out.plane[0][i] = (float)((word >> 10) & 0x3FFu) / 1023.0f;
            out.plane[1][i] = (float)(word & 0x3FFu) / 1023.0f;
            out.plane[2][i] = (float)((word >> 20) & 0x3FFu) / 1023.0f;
        }
        return true;
    }

    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(toVkFormat(format));
    if ((mpInfo == nullptr) || (mpInfo->planesLayout.numberOfExtraPlanes == 0)) {
        return false;   // RGBA and the other packed formats: not modelled here
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

VkResult FilterTestApp::init(bool verbose, bool validate, const char* deviceUuidStr, bool descriptorBuffer) {
    
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

    // --descriptor-buffer: the same, with descriptor buffers in place of push descriptors. A device
    // without VK_KHR_push_descriptor takes descriptor buffers (VulkanDeviceContext::UsesDescriptorBuffers).
    static const char* const requiredDescriptorBufferDeviceExtensions[] = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        nullptr
    };
    
    // Optional extensions
    static const char* const optionalDeviceExtensions[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        nullptr
    };
    
    // Enable the validation layers for --validate as well as --verbose. --validate is
    // the CI-usable form: it turns the layers on without the verbose log firehose, so
    // running with validation does not depend on also wanting the extra logging.
    const bool enableValidation = (verbose || validate);
    if (enableValidation) {
        m_vkDevCtx.AddReqInstanceLayers(requiredInstanceLayers);
        m_vkDevCtx.AddReqInstanceExtensions(requiredInstanceExtensions);
    }
    
    // Add required device extensions
    m_vkDevCtx.AddReqDeviceExtensions(descriptorBuffer ? requiredDescriptorBufferDeviceExtensions
                                                       : requiredDeviceExtensions, verbose);
    m_vkDevCtx.AddOptDeviceExtensions(optionalDeviceExtensions, verbose);
    
    // Initialize Vulkan device (creates instance)
    VkResult result = m_vkDevCtx.InitVulkanDevice("VkFilterTest", VK_NULL_HANDLE, verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "[FilterTestApp] Failed to initialize Vulkan device: " << result << std::endl;
        return result;
    }
    
    // Initialize debug report (only if validation is enabled)
    result = m_vkDevCtx.InitDebugReport(enableValidation, verbose);
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

    // --descriptor-buffer tests nothing on a device that does not take descriptor buffers.
    if (descriptorBuffer && !m_vkDevCtx.UsesDescriptorBuffers()) {
        std::cerr << "[FilterTestApp] The device lacks the descriptorBuffer or bufferDeviceAddress feature"
                  << std::endl;
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
    std::cout << "  Descriptors: "
              << (m_vkDevCtx.UsesDescriptorBuffers() ? "descriptor buffers" : "push descriptors") << std::endl;
    
    return VK_SUCCESS;
}

void FilterTestApp::registerTest(const TestCaseConfig& config) {
    m_testCases.push_back(config);
}

namespace {
// -----------------------------------------------------------------------
// Registry of cases whose output is genuinely read back, genuinely compared,
// and genuinely DISAGREES -- because of a defect in the FILTER, not in this
// harness.
//
// Entries are pinned here rather than silenced, because the two ways of
// making the suite green again are both worse: dropping the comparison
// rebuilds the exact check-that-cannot-fail this harness exists to remove,
// and letting the suite go red on arrival gets the whole job ignored. So a
// listed case that FAILS is reported XFAIL and does not sink the run -- and a
// listed case that PASSES is a hard FAILURE, so a fix cannot land without
// this list being updated.
//
// The list is EMPTY. It held two entries, both removed when the single filter
// defect behind them was fixed:
//
//   TC043_YCbCrCopy_NV16   -- YCBCRCOPY to 4:2:2 wrote only the top half of
//                             the chroma plane (rows 540..1079 all zero,
//                             1032750 of 2073600 chroma bytes wrong).
//   TC044_YCbCrCopy_YUV444 -- YCBCRCOPY to 4:4:4 decimated chroma 2x2 (Cb
//                             read 72,72,106,106,... for an input stepping
//                             by 17, row 1 a duplicate of row 0; all 2073600
//                             Cb bytes wrong).
//
// One cause, two faces: InitYCBCRCOPY() hardcoded the shader's luma block to
// 2x2 while the host dispatch was already sized from the OUTPUT format's
// chroma subsampling, so shader and dispatch disagreed for every non-4:2:0
// output. See the comment at that call site in VulkanFilterYuvCompute.cpp.
// Both planes are now byte-exact against the CPU reference on an A4000
// (maxdiff=0, psnr=100), and the XPASS arm below is what forced this list to
// be updated in the same change.
//
// Keep the mechanism: the next real filter defect belongs in here, not in a
// deleted assertion.
// -----------------------------------------------------------------------
struct KnownFilterDefect {
    const char* testName;
    const char* reason;
};

// std::vector, not a C array: a zero-length array is not standard C++, and
// the whole point of this list is that it is allowed to be empty.
const std::vector<KnownFilterDefect> kKnownFilterDefects = {
};

const KnownFilterDefect* FindKnownFilterDefect(const std::string& name) {
    for (const auto& entry : kKnownFilterDefects) {
        if (name == entry.testName) {
            return &entry;
        }
    }
    return nullptr;
}
}  // namespace

// Per-plane description of the packed host pattern buffer. Mirrors exactly
// the layout generateTestPattern() produces and calculateImageSize()
// measures: planes back to back, each tightly packed, no padding.
namespace {
struct PlaneCopyDesc {
    VkImageAspectFlagBits aspect;
    uint32_t              width;       // plane width, in plane texels
    uint32_t              height;      // plane height, in plane texels
    uint32_t              texelBytes;  // bytes per plane texel
};

uint32_t describePlanes(TestFormat format, uint32_t width, uint32_t height,
                        PlaneCopyDesc planes[3]) {
    switch (format) {
        case TestFormat::RGBA8:
        case TestFormat::BGRA8:
        case TestFormat::Y410:   // packed 4:4:4, single plane
            planes[0] = {VK_IMAGE_ASPECT_COLOR_BIT, width, height, 4};
            return 1;
        case TestFormat::NV12:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 1};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width / 2, height / 2, 2};
            return 2;
        case TestFormat::P010:
        case TestFormat::P012:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 2};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width / 2, height / 2, 4};
            return 2;
        case TestFormat::I420:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 1};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width / 2, height / 2, 1};
            planes[2] = {VK_IMAGE_ASPECT_PLANE_2_BIT, width / 2, height / 2, 1};
            return 3;
        case TestFormat::NV16:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 1};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width / 2, height, 2};
            return 2;
        case TestFormat::P210:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 2};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width / 2, height, 4};
            return 2;
        case TestFormat::YUV444:
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, width, height, 1};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, width, height, 1};
            planes[2] = {VK_IMAGE_ASPECT_PLANE_2_BIT, width, height, 1};
            return 3;
        default:
            return 0;
    }
}
}  // namespace

TestResult FilterTestApp::runTest(const TestCaseConfig& config) {
    TestResult result;
    result.testName = config.name;
    
    auto startTime = std::chrono::high_resolution_clock::now();

    // VkImage handles are recycled once the previous test released its
    // resources, so a stale entry here would alias an unrelated image.
    m_hostUploadedOptimal.clear();

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
    std::vector<uint8_t> firstInputPattern;
    std::vector<uint8_t> firstInputReference;

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
        
        // Generate test pattern. Capture the first input twice: the staged bytes,
        // which are uploaded verbatim, and the same picture in logical component
        // order, which is what the CPU reference conversion is computed from.
        if (inputSlot.generateTestPattern) {
            const bool isFirstInput = (&inputSlot == &config.inputs[0]);
            vkResult = generateTestPattern(inputSlot, image, buffer,
                                           isFirstInput ? &firstInputPattern   : nullptr,
                                           isFirstInput ? &firstInputReference : nullptr);
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
    
    // Create the filter. It reads the conversion's format, model and range for its color matrix, and
    // makes no conversion object.
    VkSamplerYcbcrConversionCreateInfo ycbcrInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    ycbcrInfo.format = toVkFormat(config.inputs[0].format);
    ycbcrInfo.ycbcrModel = config.ycbcrModel;
    ycbcrInfo.ycbcrRange = config.ycbcrRange;
    ycbcrInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    
    // A buffer on either side is the filter's buffer input or output.
    uint32_t filterFlags = config.filterFlags;
    if (config.inputs[0].resourceType == ResourceType::Buffer) {
        filterFlags |= VulkanFilterYuvCompute::FLAG_INPUT_BUFFER;
    }
    if (config.outputs[0].resourceType == ResourceType::Buffer) {
        filterFlags |= VulkanFilterYuvCompute::FLAG_OUTPUT_BUFFER;
    }

    // A transfer-only type copies its input unchanged, with no dispatch: CreateTransfer() makes
    // it, and the copy is recorded as the filter's post-transfer below.
    TransferOpType transferOp = TransferOpType::NONE;
    switch (config.filterType) {
        case VulkanFilterYuvCompute::XFER_IMAGE_TO_BUFFER:
            transferOp = TransferOpType::COPY_IMAGE_TO_BUFFER;
            break;
        case VulkanFilterYuvCompute::XFER_BUFFER_TO_IMAGE:
            transferOp = TransferOpType::COPY_BUFFER_TO_IMAGE;
            break;
        case VulkanFilterYuvCompute::XFER_IMAGE_TO_IMAGE:
            transferOp = TransferOpType::COPY_IMAGE_TO_IMAGE;
            break;
        default:
            break;
    }

    VkSharedBaseObj<VulkanFilter> filter;
    VkResult vkResult = VK_SUCCESS;
    if (transferOp != TransferOpType::NONE) {
        vkResult = VulkanFilterYuvCompute::CreateTransfer(&m_vkDevCtx,
                                                          m_vkDevCtx.GetComputeQueueFamilyIdx(),
                                                          0,  // queue index
                                                          transferOp,
                                                          filter);
    } else {
        vkResult = VulkanFilterYuvCompute::Create(
            &m_vkDevCtx,
            m_vkDevCtx.GetComputeQueueFamilyIdx(),
            0,  // queue index
            config.filterType,
            4,  // maxNumFrames
            toVkFormat(config.inputs[0].format),
            toVkFormat(config.outputs[0].format),
            filterFlags,
            &ycbcrInfo,
            nullptr,  // YCbCr primaries constants (use default)
            nullptr,  // Sampler create info (the filter makes no sampler)
            filter
        );
    }

    if (vkResult != VK_SUCCESS) {
        result.errorMessage = "Failed to create filter: " + std::to_string(vkResult);
        result.passed = false;
        // A filter the device cannot run (a storage image format it cannot read, a buffer
        // without the 8- or 16-bit storage it needs) is not coverage either, as for a format
        // it does not implement.
        result.unvalidated = (vkResult == VK_ERROR_FORMAT_NOT_SUPPORTED) ||
                             (vkResult == VK_ERROR_FEATURE_NOT_PRESENT);
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

    // ---- Put every filter image into VK_IMAGE_LAYOUT_GENERAL. ----
    //
    // This harness recorded NO layout transition at all, so every image
    // reached vkCmdDispatch in the layout it was created in, while the
    // filter's descriptors -- correctly -- declare GENERAL, the only layout a
    // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE descriptor admits
    // (VUID-VkDescriptorImageInfo-imageLayout-00344). That is
    // VUID-vkCmdDraw-None-09600, 501 per --all run, and it is a defect in the
    // TEST, not in the filter: the filter is handed images by its caller and
    // cannot know what layout they are in. The encoder's caller
    // (VkVideoEncoder::StageInputFrame) records exactly these two barriers.
    //
    // oldLayout is read from the create info rather than assumed, so the
    // PREINITIALIZED (host-written LINEAR) and UNDEFINED (device-local
    // OPTIMAL) cases each name the layout the image is actually in. Naming
    // UNDEFINED for a PREINITIALIZED image would validate, and would discard
    // the test pattern -- the failure this exists to avoid, not to trade for.
    {
        std::vector<VkImageMemoryBarrier> toGeneral;
        auto addBarrier = [&toGeneral, this](const VkSharedBaseObj<VkImageResource>& img) {
            if (img == nullptr) {
                return;
            }
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            // The pattern uploads and the transfer-only cases copy into these images too.
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = img->GetImageCreateInfo().initialLayout;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img->GetImage();
            // COLOR_BIT covers every plane of a multi-planar image, which is
            // what the per-plane storage views are built over.
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toGeneral.push_back(b);
        };
        for (const auto& img : inputImages) {
            addBarrier(img);
        }
        for (const auto& img : outputImages) {
            addBarrier(img);
        }
        if (!toGeneral.empty()) {
            m_vkDevCtx.CmdPipelineBarrier(cmdBuffer,
                                          VK_PIPELINE_STAGE_HOST_BIT,
                                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0, nullptr,
                                          0, nullptr,
                                          (uint32_t)toGeneral.size(),
                                          toGeneral.data());
        }
    }
    
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
    if (transferOp != TransferOpType::NONE) {
        // The filter's transfer from the test's input to its output, recorded as the post-transfer
        // of an output slot whose primary resource is the input: the filter records no dispatch.
        // An image input is uploaded first and moved to TRANSFER_SRC_OPTIMAL; an image output is
        // in GENERAL from the barrier above, and returns there for its readback.
        const TestIOSlot& inSlot  = config.inputs[0];
        const TestIOSlot& outSlot = config.outputs[0];
        auto bufferResource = [](const TestIOSlot& slot, const VkSharedBaseObj<VkBufferResource>& buffer) {
            PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
            const uint32_t numPlanes = calculatePlaneGeometry(slot.format, slot.width, slot.height, geometry);
            return TransferResource::fromBuffer(buffer->GetBuffer(),
                                                calculateImageSize(slot.format, slot.width, slot.height),
                                                numPlanes, geometry);
        };
        auto transferBarrier = [this, cmdBuffer](VkImage image, VkImageLayout oldLayout,
                                                 VkImageLayout newLayout) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            m_vkDevCtx.CmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                          0, nullptr, 0, nullptr, 1, &barrier);
        };

        TransferResource src;
        if (inSlot.resourceType == ResourceType::Image) {
            vkResult = recordPatternUpload(cmdBuffer, inSlot, inputImages[0], firstInputPattern,
                                           inputStagingBuffer);
            if (vkResult == VK_SUCCESS) {
                transferBarrier(inputImages[0]->GetImage(), VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                src = TransferResource::fromImageResource(*inputImages[0],
                                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
        } else {
            src = bufferResource(inSlot, inputBuffers[0]);
        }

        if (vkResult == VK_SUCCESS) {
            FilterExecutionDesc execDesc{};
            execDesc.numInputs  = 0;
            execDesc.numOutputs = 1;
            execDesc.outputs[0].primary = src;
            execDesc.outputs[0].postTransferDest =
                (outSlot.resourceType == ResourceType::Image)
                    ? TransferResource::fromImageResource(*outputImages[0], VK_IMAGE_LAYOUT_GENERAL)
                    : bufferResource(outSlot, outputBuffers[0]);
            execDesc.outputs[0].postTransferOp = transferOp;
            vkResult = yuvFilter->RecordCommandBuffer(cmdBuffer, 0 /* bufferIdx */, execDesc);
        }

        if ((vkResult == VK_SUCCESS) && (outSlot.resourceType == ResourceType::Image)) {
            transferBarrier(outputImages[0]->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL);
        }
    } else if (config.inputs[0].resourceType == ResourceType::Image &&
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

            // Bind the subsampled-Y target when the filter was built with Y subsampling.
            //
            // FLAG_ENABLE_Y_SUBSAMPLING makes the generated shader declare and statically
            // use binding 9 (subsampledImageY). The filter reads that image from
            // execDesc.outputs[1] -- see RecordComputeDispatch. numOutputs must therefore
            // be 2 whenever the flag is set: dispatching with binding 9 unwritten is
            // VUID-vkCmdDispatch-None-08114, and the case still reports PASS on its own
            // checks, because writes to an unbound descriptor are discarded.
            //
            // The subsampled image is the LAST configured output (TC091 appends it to the
            // dual-output config), while the filter wants it in slot 1, so map explicitly
            // rather than by position.
            if ((config.filterFlags & VulkanFilterYuvCompute::FLAG_ENABLE_Y_SUBSAMPLING) &&
                (outputImages.size() > 1)) {
                const size_t subIdx = outputImages.size() - 1;
                execDesc.numOutputs = 2;
                execDesc.outputs[1].primary =
                    TransferResource::fromImageResource(*outputImages[subIdx],
                                                        VK_IMAGE_LAYOUT_UNDEFINED);
                execDesc.outputs[1].primaryView = outputImageViews[subIdx].get();
            }

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
        // A buffer on either side: its planes tightly packed, as generateTestPattern() writes a
        // buffer input and validateOutput() reads a buffer output. The filter takes each plane's
        // offset and row pitch in bytes.
        const TestIOSlot& inSlot  = config.inputs[0];
        const TestIOSlot& outSlot = config.outputs[0];
        auto planeLayouts = [](const TestIOSlot& slot, VkSubresourceLayout layouts[3]) -> uint32_t {
            PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
            const uint32_t numPlanes = calculatePlaneGeometry(slot.format, slot.width, slot.height, geometry);
            for (uint32_t i = 0; (i < numPlanes) && (i < 3); i++) {
                layouts[i] = VkSubresourceLayout{};
                layouts[i].offset = geometry[i].offset;
                layouts[i].size = geometry[i].size;
                layouts[i].rowPitch = geometry[i].rowPitch;
                layouts[i].arrayPitch = geometry[i].size;
            }
            return numPlanes;
        };
        VkSubresourceLayout inLayouts[3]{};
        VkSubresourceLayout outLayouts[3]{};
        const uint32_t numInPlanes = planeLayouts(inSlot, inLayouts);
        const uint32_t numOutPlanes = planeLayouts(outSlot, outLayouts);
        const VkFormat inFormats[3] = {toVkFormat(inSlot.format), toVkFormat(inSlot.format), toVkFormat(inSlot.format)};
        const VkFormat outFormats[3] = {toVkFormat(outSlot.format), toVkFormat(outSlot.format), toVkFormat(outSlot.format)};

        if (inSlot.resourceType == ResourceType::Image) {
            // Image input, buffer output: upload the pattern into the input image first, a copy
            // per plane from a staging buffer, into the GENERAL layout the barrier above set.
            vkResult = recordPatternUpload(cmdBuffer, inSlot, inputImages[0], firstInputPattern,
                                           inputStagingBuffer);
            if (vkResult != VK_SUCCESS) {
                result.errorMessage = "Failed to stage the input image's pattern";
                result.passed = false;
                m_vkDevCtx.EndCommandBuffer(cmdBuffer);
                m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);
                return result;
            }
            VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShader.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toShader.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.image = inputImages[0]->GetImage();
            toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            m_vkDevCtx.CmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                          0, nullptr, 0, nullptr, 1, &toShader);

            const VkBuffer outBuffer = outputBuffers[0]->GetBuffer();
            VkBufferImageCopy outRegion{};
            outRegion.imageExtent = {outSlot.width, outSlot.height, 1};
            vkResult = yuvFilter->RecordCommandBuffer(cmdBuffer, 0, inputImageViews[0].get(), &inputResourceInfo,
                                                      &outBuffer, 1, outFormats, outLayouts, numOutPlanes,
                                                      &outRegion);
        } else if (outSlot.resourceType == ResourceType::Image) {
            // Buffer input, image output: the output is read back after the submission.
            const VkBuffer inBuffer = inputBuffers[0]->GetBuffer();
            VkBufferImageCopy inRegion{};
            inRegion.imageExtent = {inSlot.width, inSlot.height, 1};
            vkResult = yuvFilter->RecordCommandBuffer(cmdBuffer, 0, &inBuffer, 1, inFormats, inLayouts, numInPlanes,
                                                      outputImageViews[0].get(), &outputResourceInfo, &inRegion);
        } else {
            const VkBuffer inBuffer = inputBuffers[0]->GetBuffer();
            const VkBuffer outBuffer = outputBuffers[0]->GetBuffer();
            vkResult = yuvFilter->RecordCommandBuffer(cmdBuffer, 0,
                                                      &inBuffer, 1, inFormats, inLayouts, numInPlanes,
                                                      VkExtent3D{inSlot.width, inSlot.height, 1},
                                                      &outBuffer, 1, outFormats, outLayouts, numOutPlanes,
                                                      VkExtent3D{outSlot.width, outSlot.height, 1});
        }
        if ((vkResult == VK_SUCCESS) && (outSlot.resourceType == ResourceType::Buffer)) {
            // The shader's writes to the output buffer, made visible to the host that reads it.
            VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            m_vkDevCtx.CmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
        }
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

    // The output image of a buffer input or of a transfer-only filter: read back now, as the image
    // cases' post-transfer does in their submission.
    if (((config.inputs[0].resourceType == ResourceType::Buffer) || (transferOp != TransferOpType::NONE)) &&
        (config.outputs[0].resourceType == ResourceType::Image) && config.outputs[0].validateOutput) {
        vkResult = copyImageToStagingBuffer(config.outputs[0], outputImages[0], outputStagingBuffer);
        if (vkResult != VK_SUCCESS) {
            result.errorMessage = "Failed to read back the output image: " + std::to_string(vkResult);
            result.passed = false;
            return result;
        }
    }
    
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
                generateReferenceOutput(config, firstInputReference);

            if (referenceData.empty() && !firstInputReference.empty()) {
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
    
    // A case that is REQUIRED to disagree with the reference. See
    // TestCaseConfig::expectReferenceMismatch. Applied before the
    // known-defect registry because it is a property of the configuration,
    // not a defect awaiting a fix.
    if (config.expectReferenceMismatch) {
        if (result.passed) {
            std::cout << "[UNEXPECTED-MATCH] " << config.name
                      << ": this configuration cannot be colour-correct, yet it "
                         "matched the reference." << std::endl;
            result.errorMessage =
                "Expected a reference MISMATCH (the configuration under test "
                "cannot reproduce the source colours) but the output matched. "
                "Re-derive the premise before trusting this.";
            result.passed = false;
        } else {
            std::cout << "[EXPECTED-MISMATCH] " << config.name
                      << ": disagreed with the reference, as required (observed: "
                      << result.errorMessage << ")" << std::endl;
            result.errorMessage =
                "Expected mismatch confirmed: " + result.errorMessage;
            result.passed = true;
        }
    }

    // Known filter defects: XFAIL is tolerated, XPASS is not. See
    // kKnownFilterDefects above for why this is a registry and not a mute.
    const KnownFilterDefect* known = FindKnownFilterDefect(config.name);
    if (known != nullptr) {
        if (!result.passed) {
            std::cout << "[XFAIL] " << config.name << ": known filter defect -- "
                      << known->reason << " (observed: " << result.errorMessage << ")"
                      << std::endl;
            result.errorMessage = "XFAIL (known filter defect): " + std::string(known->reason);
            result.passed = true;
        } else {
            std::cout << "[XPASS] " << config.name
                      << ": listed in kKnownFilterDefects but PASSED." << std::endl;
            result.errorMessage =
                "XPASS: this case is listed in kKnownFilterDefects but now passes. "
                "If the filter was fixed, delete its entry from that list.";
            result.passed = false;
        }
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
        // A run with nothing validated is not a green run, whatever the failure
        // count says: a suite can report every case passing while checking no
        // pixels at all. Call it out.
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
        
        // The compute filter binds every image slot it is handed -- the
        // single-plane combined view and the per-plane YCbCr views alike --
        // as a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, so storage is the only
        // access form the views have to carry. The usage a view requests must
        // be a subset of the usage its image was created with
        // (VUID-VkImageViewCreateInfo-pNext-02662), which is what ties this
        // to |kTestImageUsage|: the two are one declaration expressed twice
        // and must be read together.
        result = VkImageResourceView::Create(&m_vkDevCtx, outImage, subresRange,
                                             VK_IMAGE_USAGE_STORAGE_BIT,
                                             outImageView);
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
                                           std::vector<uint8_t>* pOutPatternData,
                                           std::vector<uint8_t>* pOutReferencePattern) {
    std::vector<uint8_t> patternData;
    
    // Generate test pattern based on input format
    switch (slot.format) {
        case TestFormat::RGBA8:
        case TestFormat::BGRA8: {
            // Generate the pattern in LOGICAL R,G,B,A byte order for BOTH
            // formats. The BGRA staging swap happens below, after
            // |outPatternData| has been handed the logical bytes -- see the
            // comment there for why the two must differ.
            generateRGBATestPattern(slot.pattern,
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
        case TestFormat::P210:
        case TestFormat::P212: {
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
            } else if (slot.format == TestFormat::P212) {
                convertRGBAtoP212(rgbaData.data(), slot.width, slot.height,
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

        case TestFormat::Y410: {
            std::vector<uint8_t> rgbaData;
            generateRGBATestPattern(TestPatternType::ColorBars,
                                   slot.width, slot.height, rgbaData);
            convertRGBAtoY410(rgbaData.data(), slot.width, slot.height,
                              ColorPrimaries::BT709, ColorRange::Full, patternData);
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
    
    // BGRA8: stage the SAME PICTURE, written the way that format spells it.
    //
    // This is the whole point of the BGRA cases, so it is worth being exact
    // about what is being asserted. The pattern above is in logical R,G,B,A
    // byte order. A VK_FORMAT_B8G8R8A8_UNORM image spells the identical colour
    // with bytes 0 and 2 exchanged. So:
    //
    //   * |patternData| (swapped below) is uploaded -- the bytes a real BGRA
    //     producer would hand us.
    //   * |outPatternData| keeps the LOGICAL RGBA bytes, and the CPU reference
    //     is computed from those.
    //
    // Consequence, and this is the test: a correct implementation must produce
    // BYTE-IDENTICAL YCbCr for the RGBA8 and BGRA8 cases, because they are the
    // same picture. An implementation that reads BGRA memory as if it were
    // RGBA -- exactly what a `rgba8` storage qualifier on a BGRA view does --
    // produces the red/blue-swapped picture and fails against the shared
    // reference. Feeding the swapped bytes to a swapped reference would cancel
    // the two errors out and assert nothing at all.
    const bool stageAsBgra = (slot.format == TestFormat::BGRA8) && slot.bgraStageSwap;
    std::vector<uint8_t> logicalRgbaPattern;
    if (stageAsBgra && !patternData.empty()) {
        logicalRgbaPattern = patternData;
        for (size_t i = 0; (i + 3) < patternData.size(); i += 4) {
            std::swap(patternData[i + 0], patternData[i + 2]);
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
    // copy has to be in the same submission as the compute dispatch. A staging
    // buffer filled and then dropped on return, with the copy left undone, feeds
    // the shader uninitialised memory while the case still reports success --
    // which is why the buffer's lifetime and the submission are arranged here
    // rather than locally.

    // Note: linear images are NOT written through their host mapping here. See the
    // staging path in runTest() -- a direct memcpy assumes the planes are tightly packed,
    // which is not true on every driver.

    // Two answers, and they differ for exactly one slot format. Handing back a single
    // vector for both jobs is wrong for BGRA8 in whichever direction it is resolved.
    //
    //   |pOutPatternData|      the bytes AS STAGED. The upload copies these into the
    //                          image, so this is the picture the shader really reads,
    //                          spelled the way the slot's VkFormat spells it.
    //   |pOutReferencePattern| the same picture in LOGICAL R,G,B,A order. The reference
    //                          generators read pixel[0] as red, so giving them the
    //                          exchanged bytes would describe the byte order instead of
    //                          the picture -- and the exchange would cancel against
    //                          itself, leaving the comparison asserting nothing.
    //
    // They coincide for every format that stages its bytes in logical order, which is
    // every format except a BGRA8 slot with bgraStageSwap set.
    if (pOutPatternData != nullptr) {
        *pOutPatternData = patternData;
    }
    if (pOutReferencePattern != nullptr) {
        *pOutReferencePattern = stageAsBgra ? logicalRgbaPattern : patternData;
    }

    return VK_SUCCESS;
}

// A multi-planar YCbCr output against its reference one component at a time: Y, and Cb and Cr apart
// whether they share a plane or not, in the format's sample units (16-bit words for the 10- and 12-bit
// formats). Each component must reach 30 dB and stay within twice the tolerance, as compareNV12()
// requires of NV12: an error in one chroma component must not hide in an average over the frame.
static ValidationResult compareYcbcrComponents(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                                               TestFormat format, uint32_t width, uint32_t height, float tolerance) {
    ValidationResult result;
    PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
    const uint32_t numPlanes = calculatePlaneGeometry(format, width, height, geometry);
    const size_t frameSize = (size_t)(geometry[numPlanes - 1].offset + geometry[numPlanes - 1].size);
    if ((numPlanes < 2) || (actual.size() < frameSize) || (expected.size() < frameSize)) {
        result.passed = false;
        result.errorMessage = "Size mismatch for " + std::string(testFormatName(format)) + " validation";
        return result;
    }

    const bool is16Bit = formatIs16BitSamples(format);
    const double peak = is16Bit ? 65535.0 : 255.0;
    const uint32_t maxAllowed = static_cast<uint32_t>(tolerance * peak) * 2;
    auto sample = [is16Bit](const std::vector<uint8_t>& bytes, size_t byteOffset) {
        return is16Bit ? (int)(bytes[byteOffset] | (bytes[byteOffset + 1] << 8)) : (int)bytes[byteOffset];
    };

    // Component 0 is Y (plane 0), 1 Cb and 2 Cr: plane 1 and 2 of a 3-plane format, or the even and odd
    // samples of plane 1 of a 2-plane one.
    double sumSq[3] = {0.0, 0.0, 0.0};
    double maxError[3] = {0.0, 0.0, 0.0};
    size_t count[3] = {0, 0, 0};
    const size_t sampleBytes = is16Bit ? 2 : 1;
    for (uint32_t plane = 0; plane < numPlanes; plane++) {
        const size_t samples = (size_t)geometry[plane].size / sampleBytes;
        for (size_t i = 0; i < samples; i++) {
            const uint32_t component = (plane == 0) ? 0 : ((numPlanes == 3) ? plane : 1 + (uint32_t)(i & 1));
            const size_t byteOffset = (size_t)geometry[plane].offset + i * sampleBytes;
            const int d = sample(actual, byteOffset) - sample(expected, byteOffset);
            sumSq[component] += (double)d * (double)d;
            maxError[component] = std::max(maxError[component], (double)std::abs(d));
            count[component]++;
        }
    }
    double psnr[3];
    for (uint32_t c = 0; c < 3; c++) {
        const double mse = (count[c] != 0) ? (sumSq[c] / (double)count[c]) : 0.0;
        psnr[c] = (mse == 0.0) ? 100.0 : 10.0 * std::log10((peak * peak) / mse);
    }
    result.psnrY = psnr[0];
    result.psnrCb = psnr[1];
    result.psnrCr = psnr[2];
    result.maxErrorY = maxError[0];
    result.maxErrorCb = maxError[1];
    result.maxErrorCr = maxError[2];
    result.passed = (psnr[0] >= 30.0) && (psnr[1] >= 30.0) && (psnr[2] >= 30.0) && (maxError[0] <= maxAllowed) &&
                    (maxError[1] <= maxAllowed) && (maxError[2] <= maxAllowed);
    if (!result.passed) {
        char message[256];
        snprintf(message, sizeof(message),
                 "PSNR: Y=%.2f dB, Cb=%.2f dB, Cr=%.2f dB; Max errors: Y=%.0f, Cb=%.0f, Cr=%.0f (allowed %u)", psnr[0],
                 psnr[1], psnr[2], maxError[0], maxError[1], maxError[2], maxAllowed);
        result.errorMessage = message;
    }
    return result;
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
    
    // --- Readback fingerprint -------------------------------------------
    // The reference-comparison arm below is only reached when a caller
    // supplies reference bytes, and runTest() passes an empty vector, so a
    // PASS from this function says "some data came back", never "the right
    // data came back". That is not enough to tell a working descriptor arm
    // from one that binds a never-written descriptor set, nor a BT.709
    // matrix from a BT.2020 one -- both still produce a full buffer.
    //
    // So emit a checksum of the raw readback unconditionally, and dump the
    // bytes when VKFT_DUMP names a directory. An A/B that must change (a
    // matrix or range fix) and an A/B that must NOT change (swapping the
    // descriptor arm) are then both decidable from the same output.
    if (!actualData.empty()) {
        unsigned long long sum = 0;
        unsigned int fnv = 2166136261u;
        for (size_t i = 0; i < actualData.size(); i++) {
            sum += actualData[i];
            fnv = (fnv ^ actualData[i]) * 16777619u;
        }
        std::cout << "[CHECKSUM] " << config.name
                  << " bytes=" << actualData.size()
                  << " sum=" << sum
                  << " fnv1a=" << fnv << std::endl;

        const char* dumpDir = getenv("VKFT_DUMP");
        if (dumpDir && dumpDir[0]) {
            std::string path = std::string(dumpDir) + "/" + config.name + ".bin";
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(actualData.data(), 1, actualData.size(), f);
                fclose(f);
                std::cout << "[DUMP] " << path << std::endl;
            } else {
                std::cerr << "[DUMP] FAILED to open " << path << std::endl;
            }
        }
    }

    // Report the raw agreement between GPU output and CPU reference before
    // any tolerance is applied, so a threshold argument can be had against
    // numbers rather than against a bare PASS/FAIL.
    if (!referenceData.empty() && !actualData.empty() && (outputSlot.format != TestFormat::Y410)) {
        // Report in the format's OWN sample units (Y410's arm below reports its own). Reporting a 16-bit-per-
        // sample format byte-wise made this line disagree with the verdict it
        // is supposed to explain: TC002_RGBA_to_P010 printed "maxdiff=255
        // psnr=10.26" and then PASSED, because a 107/65535 disagreement lands
        // almost entirely in the low byte and reads as a 255-unit byte error.
        // A diagnostic that contradicts the gate is worse than none.
        const bool is16Bit = formatIs16BitSamples(outputSlot.format);
        const size_t bytes = std::min(actualData.size(), referenceData.size());
        const size_t n = is16Bit ? (bytes / 2) : bytes;
        const uint8_t* ab = actualData.data();
        const uint8_t* rb = referenceData.data();
        uint32_t maxDiff = 0;
        double sumSq = 0.0;
        for (size_t i = 0; i < n; i++) {
            const int av = is16Bit ? (ab[2 * i] | (ab[2 * i + 1] << 8)) : (int)ab[i];
            const int rv = is16Bit ? (rb[2 * i] | (rb[2 * i + 1] << 8)) : (int)rb[i];
            const int d = av - rv;
            const uint32_t ad = (uint32_t)(d < 0 ? -d : d);
            if (ad > maxDiff) {
                maxDiff = ad;
            }
            sumSq += (double)d * (double)d;
        }
        const double peak = is16Bit ? 65535.0 : 255.0;
        const double mse = (n != 0) ? (sumSq / (double)n) : 0.0;
        const double psnr = (mse == 0.0) ? 100.0 : 10.0 * std::log10((peak * peak) / mse);
        std::cout << "[REFCMP] " << config.name
                  << " unit=" << (is16Bit ? "u16" : "u8")
                  << " n=" << n
                  << " maxdiff=" << maxDiff
                  << " psnr=" << psnr << std::endl;
    }

    // If we have reference data, compare
    if (!referenceData.empty() && !actualData.empty()) {
        const size_t compareBytes = std::min(actualData.size(), referenceData.size());
        // VKFT_DIFF: where each plane's bytes differ from the reference, to place an error.
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
        // Determine comparison method based on output format
        switch (outputSlot.format) {
            case TestFormat::NV12:
            case TestFormat::I420:
            case TestFormat::NV16:
            case TestFormat::YUV444:
            case TestFormat::P010:
            case TestFormat::P012:
            case TestFormat::P210:
            case TestFormat::P212: {
                ValidationResult valResult = compareYcbcrComponents(actualData, referenceData, outputSlot.format,
                                                                    outputSlot.width, outputSlot.height,
                                                                    config.tolerance);
                result.passed = valResult.passed;
                result.psnrY = valResult.psnrY;
                result.psnrCb = valResult.psnrCb;
                result.psnrCr = valResult.psnrCr;
                result.errorMessage = valResult.errorMessage;
                break;
            }
            
            case TestFormat::Y410: {
                // Packed 4:4:4: compare the three 10-bit samples of each word, not its bytes.
                // Bits 9:0 carry Cb, 19:10 Y and 29:20 Cr.
                const size_t words = std::min(actualData.size(), referenceData.size()) / 4;
                double sumSq[3] = {0.0, 0.0, 0.0};
                uint32_t maxDiff[3] = {0, 0, 0};
                for (size_t i = 0; i < words; i++) {
                    uint32_t actualWord = 0;
                    uint32_t refWord = 0;
                    memcpy(&actualWord, &actualData[i * 4], sizeof(actualWord));
                    memcpy(&refWord, &referenceData[i * 4], sizeof(refWord));
                    for (uint32_t c = 0; c < 3; c++) {
                        const int d = (int)((actualWord >> (10 * c)) & 0x3FF) - (int)((refWord >> (10 * c)) & 0x3FF);
                        maxDiff[c] = std::max(maxDiff[c], (uint32_t)(d < 0 ? -d : d));
                        sumSq[c] += (double)d * (double)d;
                    }
                }
                double psnr[3];
                for (uint32_t c = 0; c < 3; c++) {
                    const double mse = (words != 0) ? (sumSq[c] / (double)words) : 0.0;
                    psnr[c] = (mse == 0.0) ? 100.0 : 10.0 * std::log10((1023.0 * 1023.0) / mse);
                }
                std::cout << "[REFCMP] " << config.name << " unit=y410 n=" << words
                          << " maxdiff Y/Cb/Cr=" << maxDiff[1] << "/" << maxDiff[0] << "/" << maxDiff[2]
                          << " psnr Y/Cb/Cr=" << psnr[1] << "/" << psnr[0] << "/" << psnr[2] << std::endl;
                result.psnrCb = psnr[0];
                result.psnrY = psnr[1];
                result.psnrCr = psnr[2];
                result.passed = ((words * 4) == expectedSize) &&
                                (psnr[0] >= 30.0) && (psnr[1] >= 30.0) && (psnr[2] >= 30.0);
                if (!result.passed) {
                    result.errorMessage = "Y410 PSNR below threshold or size mismatch: Y " + std::to_string(psnr[1]) +
                                          " dB, Cb " + std::to_string(psnr[0]) +
                                          " dB, Cr " + std::to_string(psnr[2]) + " dB";
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
                // 10/12-bit formats store one sample per 16-bit word, compared as such.
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

                    case TestFormat::Y410:
                        convertRGBAtoY410(inputData.data(), input.width, input.height,
                                          primaries, range, referenceData);
                        break;

                    case TestFormat::P010:
                    case TestFormat::P012:
                    case TestFormat::P210:
                    case TestFormat::P212: {
                        std::vector<uint16_t> yPlane16, uvPlane16;
                        if (output.format == TestFormat::P210) {
                            convertRGBAtoP210(inputData.data(), input.width, input.height,
                                              primaries, range, yPlane16, uvPlane16);
                        } else if (output.format == TestFormat::P212) {
                            convertRGBAtoP212(inputData.data(), input.width, input.height,
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
                    
                    default: {
                        // Generic model for any YCbCr input unpackToCanonical() reads: planar,
                        // semi-planar or Y410. The shader fetches chroma with imageLoad at
                        // srcPos/ratio -- nearest neighbour, no filtering -- so the reference
                        // replicates chroma the same way via the canonical frame, which uses
                        // the same x/subX, y/subY indexing.
                        if (range != ColorRange::Full) {
                            break;   // limited range not modelled; stays honestly unvalidated
                        }
                        CanonicalFrame frame;
                        if (!unpackToCanonical(inputData, input.format,
                                               input.width, input.height, frame)) {
                            break;
                        }
                        referenceData.resize((size_t)input.width * input.height * 4);
                        for (size_t i = 0; i < (size_t)input.width * input.height; i++) {
                            YCbCrPixel ycbcr;
                            ycbcr.y  = frame.plane[0][i];
                            ycbcr.cb = frame.plane[1][i] - 0.5f;
                            ycbcr.cr = frame.plane[2][i] - 0.5f;
                            const RGBPixel rgb = ycbcrToRgb(ycbcr, primaries);
                            referenceData[i * 4 + 0] = (uint8_t)(rgb.r * 255.0f + 0.5f);
                            referenceData[i * 4 + 1] = (uint8_t)(rgb.g * 255.0f + 0.5f);
                            referenceData[i * 4 + 2] = (uint8_t)(rgb.b * 255.0f + 0.5f);
                            referenceData[i * 4 + 3] = 255;
                        }
                        break;
                    }
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

        case VulkanFilterYuvCompute::XFER_IMAGE_TO_BUFFER:
        case VulkanFilterYuvCompute::XFER_BUFFER_TO_IMAGE:
        case VulkanFilterYuvCompute::XFER_IMAGE_TO_IMAGE:
            // A transfer copies the picture unchanged, between the same format and extent.
            if ((input.format == output.format) &&
                (input.width == output.width) && (input.height == output.height)) {
                referenceData = inputData;
            }
            break;

        case VulkanFilterYuvCompute::YCBCRCLEAR: {
            // For clear, generate expected cleared values
            size_t size = calculateImageSize(output.format, output.width, output.height);
            referenceData.resize(size);

            if (formatIs16BitSamples(output.format)) {
                // The CLEAR shader does imageStore(..., vec4(0.5, ...)) into a
                // plane view whose storage is 16 bits per sample, so the value
                // that lands is round(0.5 * 65535) = 32767 -- measured 32767
                // on an A4000 for every luma AND chroma sample of TC051.
                // Filling BYTES with 128 made the reference 0x8080 = 32896,
                // which is not 50% of anything; it is the same units mistake
                // convertRGBAtoP010() makes, in the other direction. The
                // 16-bit comparison arm above tolerates the +-1 LSB that
                // another driver's rounding could produce (a 1/65535 error is
                // ~96 dB, far above the 30 dB gate).
                const uint16_t mid = 32767;
                for (size_t i = 0; i + 1 < size; i += 2) {
                    referenceData[i]     = (uint8_t)(mid & 0xFF);
                    referenceData[i + 1] = (uint8_t)((mid >> 8) & 0xFF);
                }
            } else {
                // 50% gray for Y=0.5 and neutral CbCr=0.5, i.e. 128 for 8-bit.
                std::fill(referenceData.begin(), referenceData.end(), 128);
            }
            break;
        }
        
        default:
            break;
    }
    
    return referenceData;
}

// ---------------------------------------------------------------------------
// Read an OPTIMAL-tiled image back into a host-visible staging buffer.
//
// This was a `return VK_SUCCESS;` stub with no callers at all, and runTest()
// took that as licence to short-circuit every optimal-tiled OUTPUT to
// passed = true WITHOUT READING A BYTE -- 46 of the 54 --all cases. The 8
// linear-output cases were the only ones validating anything. A green that
// cannot go red is the same defect the input side had, pointing the other
// way.
//
// Two things the input-side fix paid for, applied here rather than
// rediscovered:
//
//  1. oldLayout is VK_IMAGE_LAYOUT_GENERAL, NOT the create-info
//     VK_IMAGE_LAYOUT_UNDEFINED. runTest()'s barrier puts every filter image
//     into GENERAL before the dispatch, and the filter's own trailing
//     barrier (VulkanFilterYuvCompute::RecordCommandBuffer, the
//     GENERAL->GENERAL / SHADER_WRITE->MEMORY_READ one) leaves the output
//     there. Naming the create-info UNDEFINED would validate cleanly and
//     would be free to DISCARD the very pixels this function exists to read
//     -- the symmetric twin of the hazard that nearly ate the input pattern.
//
//  2. The DESTINATION packing is ours to pick, so pick tight:
//     bufferRowLength / bufferImageHeight = 0 means "tightly packed to
//     imageExtent". A copy-to-buffer does not inherit the image's row
//     padding, so this sidesteps the trap the LINEAR readback had to solve
//     with GetPlaneLayout() -- there, luma was padded 2073600 -> 2 MiB and a
//     flat read put the whole chroma plane 23552 bytes out. Here the result
//     is byte-for-byte what calculateImageSize() measures.
// ---------------------------------------------------------------------------
VkResult FilterTestApp::copyImageToStagingBuffer(const TestIOSlot& slot,
                                                 VkSharedBaseObj<VkImageResource>& image,
                                                 VkSharedBaseObj<VkBufferResource>& stagingBuffer) {
    if (image == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PlaneCopyDesc planes[3];
    const uint32_t numPlanes = describePlanes(slot.format, slot.width, slot.height, planes);
    if (numPlanes == 0) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    // Each region's bufferOffset must be a multiple of the PLANE's texel
    // size (VUID-vkCmdCopyImageToBuffer-srcImage-07976), and tight packing of
    // the earlier planes does not guarantee that: TC101 is 1921x1081, so the
    // NV12 luma plane is 2076601 bytes and plane 1's texel size is 2. So the
    // copy is recorded at ALIGNED offsets and the planes are compacted down
    // to tight packing on the host once the fence signals -- the caller still
    // gets the tightly packed buffer it expects, and the copy stays legal.
    VkDeviceSize alignedOffset[3] = {0, 0, 0};
    VkDeviceSize tightOffset[3] = {0, 0, 0};
    VkDeviceSize planeBytes[3] = {0, 0, 0};
    VkDeviceSize alignedCursor = 0;
    VkDeviceSize tightCursor = 0;
    for (uint32_t i = 0; i < numPlanes; i++) {
        const VkDeviceSize align = (planes[i].texelBytes != 0) ? planes[i].texelBytes : 1;
        alignedCursor = ((alignedCursor + align - 1) / align) * align;
        alignedOffset[i] = alignedCursor;
        tightOffset[i] = tightCursor;
        planeBytes[i] = (VkDeviceSize)planes[i].width * planes[i].height * planes[i].texelBytes;
        alignedCursor += planeBytes[i];
        tightCursor += planeBytes[i];
    }
    if (tightCursor == 0) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkResult result = createStagingBuffer((size_t)alignedCursor, stagingBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    result = m_vkDevCtx.AllocateCommandBuffers(m_vkDevCtx.getDevice(), &allocInfo, &cmdBuffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_vkDevCtx.BeginCommandBuffer(cmdBuffer, &beginInfo);

    // GENERAL, not the create-info UNDEFINED -- see (1) in the block above.
    VkImageMemoryBarrier toTransferSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.image = image->GetImage();
    // COLOR_BIT covers every plane of a multi-planar image.
    toTransferSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    m_vkDevCtx.CmdPipelineBarrier(cmdBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);

    VkBufferImageCopy regions[3]{};
    for (uint32_t i = 0; i < numPlanes; i++) {
        regions[i].bufferOffset = alignedOffset[i];
        regions[i].bufferRowLength = 0;      // tightly packed -- see (2) above
        regions[i].bufferImageHeight = 0;
        regions[i].imageSubresource.aspectMask = planes[i].aspect;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = 0;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageOffset = {0, 0, 0};
        regions[i].imageExtent = {planes[i].width, planes[i].height, 1};
    }

    m_vkDevCtx.CmdCopyImageToBuffer(cmdBuffer,
                                    image->GetImage(),
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    stagingBuffer->GetBuffer(),
                                    numPlanes, regions);

    // HOST_COHERENT memory needs no cache maintenance, but the transfer write
    // still has to be made AVAILABLE to the host domain. A fence wait alone
    // does not do that.
    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    m_vkDevCtx.CmdPipelineBarrier(cmdBuffer,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT,
                                  0, 1, &toHost, 0, nullptr, 0, nullptr);

    m_vkDevCtx.EndCommandBuffer(cmdBuffer);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    m_vkDevCtx.CreateFence(m_vkDevCtx.getDevice(), &fenceInfo, nullptr, &fence);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    result = m_vkDevCtx.QueueSubmit(m_vkDevCtx.GetComputeQueue(), 1, &submitInfo, fence);
    if (result == VK_SUCCESS) {
        result = m_vkDevCtx.WaitForFences(m_vkDevCtx.getDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
    }

    m_vkDevCtx.DestroyFence(m_vkDevCtx.getDevice(), fence, nullptr);
    m_vkDevCtx.FreeCommandBuffers(m_vkDevCtx.getDevice(), m_commandPool, 1, &cmdBuffer);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Compact the aligned plane layout down to tight packing. Only ever moves
    // bytes DOWNWARD, and only when an alignment gap was actually inserted,
    // so for every even-dimensioned case this is a no-op.
    VkDeviceSize mappedSize = 0;
    uint8_t* mapped = stagingBuffer->GetDataPtr(0, mappedSize);
    if ((mapped == nullptr) || (mappedSize < alignedCursor)) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    for (uint32_t i = 1; i < numPlanes; i++) {
        if (alignedOffset[i] != tightOffset[i]) {
            memmove(mapped + tightOffset[i], mapped + alignedOffset[i],
                    (size_t)planeBytes[i]);
        }
    }

    return VK_SUCCESS;
}

VkResult FilterTestApp::recordPatternUpload(VkCommandBuffer cmdBuffer,
                                            const TestIOSlot& slot,
                                            VkSharedBaseObj<VkImageResource>& image,
                                            const std::vector<uint8_t>& pattern,
                                            VkSharedBaseObj<VkBufferResource>& stagingBuffer) {
    PlaneBufferGeometry geometry[TransferResource::kMaxPlanes];
    const uint32_t numPlanes = calculatePlaneGeometry(slot.format, slot.width, slot.height, geometry);
    PlaneCopyDesc planes[3];
    const uint32_t numCopyPlanes = describePlanes(slot.format, slot.width, slot.height, planes);
    // Each region's bufferOffset must be a multiple of its plane's texel size, which the
    // tightly packed planes are not for every extent; such a pattern is not uploaded.
    bool aligned = (image != nullptr) && !pattern.empty() && (numCopyPlanes == numPlanes);
    for (uint32_t i = 0; aligned && (i < numPlanes); i++) {
        aligned = ((geometry[i].offset % planes[i].texelBytes) == 0);
    }
    if (!aligned) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkResult result = createStagingBuffer(pattern.size(), stagingBuffer, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkDeviceSize maxSize = 0;
    uint8_t* stagingData = stagingBuffer->GetDataPtr(0, maxSize);
    if ((stagingData == nullptr) || (maxSize < pattern.size())) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    memcpy(stagingData, pattern.data(), pattern.size());

    VkBufferImageCopy regions[3]{};
    for (uint32_t i = 0; i < numPlanes; i++) {
        regions[i].bufferOffset = geometry[i].offset;
        regions[i].imageSubresource = {static_cast<VkImageAspectFlags>(planes[i].aspect), 0, 0, 1};
        regions[i].imageExtent = {planes[i].width, planes[i].height, 1};
    }
    m_vkDevCtx.CmdCopyBufferToImage(cmdBuffer, stagingBuffer->GetBuffer(), image->GetImage(),
                                    VK_IMAGE_LAYOUT_GENERAL, numPlanes, regions);
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
