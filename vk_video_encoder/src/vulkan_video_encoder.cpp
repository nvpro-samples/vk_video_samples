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

// The encoder interface over the encoder implementation.
//
// This file is a translation layer and holds no encoder logic: every method
// forwards to the descriptor API and converts the result. The translation is
// deliberately dull, because the value of the layer is that the rules it
// enforces -- which formats resolve to which input path, which codecs carry
// HDR metadata, which reconfigurations are refusable -- are stated once here
// rather than in each caller.
//
// A role is offered only when the underlying implementation can serve it, so
// Query() returning null means the same thing to a caller whether the interface
// is the implementation or a translation onto one.

#include "vulkan_video_encoder.h"
#include "vulkan_video_encoder_ext.h"
#include "vulkan_video_encoder_ext_internal.h"
#include "vulkan_video_encoder_os_event_linux.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vk {
namespace video {
namespace enc {

namespace {

//=============================================================================
// VOCABULARY TRANSLATION
//=============================================================================

VkVideoCodecOperationFlagBitsKHR ToVkCodec(Codec codec)
{
    switch (codec) {
    case Codec::H264: return VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    case Codec::H265: return VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    case Codec::AV1:  return VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    }
    return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
}

uint32_t ToExtProfile(Profile profile)
{
    switch (profile) {
    case Profile::Default:              return VK_VIDEO_ENCODER_PROFILE_DEFAULT;
    case Profile::H264Baseline:         return VK_VIDEO_ENCODER_PROFILE_H264_BASELINE;
    case Profile::H264Main:             return VK_VIDEO_ENCODER_PROFILE_H264_MAIN;
    case Profile::H264High:             return VK_VIDEO_ENCODER_PROFILE_H264_HIGH;
    case Profile::H264High10:           return VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10;
    case Profile::H265Main:             return VK_VIDEO_ENCODER_PROFILE_H265_MAIN;
    case Profile::H265Main10:           return VK_VIDEO_ENCODER_PROFILE_H265_MAIN10;
    case Profile::H265MainStillPicture: return VK_VIDEO_ENCODER_PROFILE_H265_MAIN_STILL_PICTURE;
    case Profile::H265Rext:             return VK_VIDEO_ENCODER_PROFILE_H265_FORMAT_RANGE_EXTENSIONS;
    case Profile::AV1Main:              return VK_VIDEO_ENCODER_PROFILE_AV1_MAIN;
    case Profile::AV1High:              return VK_VIDEO_ENCODER_PROFILE_AV1_HIGH;
    case Profile::AV1Professional:      return VK_VIDEO_ENCODER_PROFILE_AV1_PROFESSIONAL;
    }
    return VK_VIDEO_ENCODER_PROFILE_DEFAULT;
}

// Which codec a profile belongs to. Default belongs to whichever codec it is
// paired with, so it is not answered here.
bool ProfileMatchesCodec(Codec codec, Profile profile)
{
    switch (profile) {
    case Profile::Default:
        return true;
    case Profile::H264Baseline:
    case Profile::H264Main:
    case Profile::H264High:
    case Profile::H264High10:
        return codec == Codec::H264;
    case Profile::H265Main:
    case Profile::H265Main10:
    case Profile::H265MainStillPicture:
    case Profile::H265Rext:
        return codec == Codec::H265;
    case Profile::AV1Main:
    case Profile::AV1High:
    case Profile::AV1Professional:
        return codec == Codec::AV1;
    }
    return false;
}

VkVideoEncoderColorModel ToExtColorModel(ColorModel model)
{
    switch (model) {
    case ColorModel::FromFormat: return VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;
    case ColorModel::Rgb:        return VK_VIDEO_ENCODER_COLOR_MODEL_RGB;
    case ColorModel::YCbCr:      return VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR;
    }
    return VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;
}

VkVideoEncodeRateControlModeFlagBitsKHR ToExtRateControl(RateControlMode mode)
{
    switch (mode) {
    case RateControlMode::Default:  return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    case RateControlMode::Disabled: return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    case RateControlMode::Cbr:      return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    case RateControlMode::Vbr:      return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    }
    return VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
}

VkVideoEncodeTuningModeKHR ToExtTuning(TuningMode mode)
{
    switch (mode) {
    case TuningMode::Default:         return VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
    case TuningMode::HighQuality:     return VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;
    case TuningMode::LowLatency:      return VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR;
    case TuningMode::UltraLowLatency: return VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR;
    case TuningMode::Lossless:        return VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR;
    }
    return VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
}

FrameState FromExtFrameState(VkVideoEncoderFrameState state)
{
    switch (state) {
    case VK_VIDEO_ENCODER_FRAME_STATE_PENDING:  return FrameState::Pending;
    case VK_VIDEO_ENCODER_FRAME_STATE_READY:    return FrameState::Ready;
    case VK_VIDEO_ENCODER_FRAME_STATE_ACQUIRED: return FrameState::Acquired;
    default:                                    return FrameState::Unknown;
    }
}

PictureType FromExtPictureType(VkVideoEncoderPictureType type)
{
    switch (type) {
    case VK_VIDEO_ENCODER_PICTURE_TYPE_P: return PictureType::Predicted;
    case VK_VIDEO_ENCODER_PICTURE_TYPE_B: return PictureType::Bidirectional;
    default:                              return PictureType::Intra;
    }
}

VkVideoEncoderInputResidency ToExtResidency(Residency residency, ExternalHandleType handleType)
{
    switch (residency) {
    case Residency::Local:   return VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    case Residency::Foreign: return VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN;
    case Residency::Auto:
        break;
    }
    // An image already resident on the encoder's device needs no ownership
    // transfer, and the library cannot infer that from the handle alone.
    // Every other handle type is an import, where AUTO is the right answer.
    return (handleType == ExternalHandleType::VkImageHandle)
               ? VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL
               : VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO;
}

// The driver's own identity for a physical device. Read through the loader
// the caller supplies, so the answer comes from the same driver the encoder
// talks to rather than whichever one is first on the path.
bool VkEncQueryDeviceUuid(VkInstance instance, VkPhysicalDevice physicalDevice,
                          PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                          uint8_t outUuid[VK_UUID_SIZE])
{
    if ((instance == VK_NULL_HANDLE) || (physicalDevice == VK_NULL_HANDLE) ||
        (getInstanceProcAddr == nullptr) || (outUuid == nullptr)) {
        return false;
    }
    PFN_vkGetPhysicalDeviceProperties2 getProps2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (getProps2 == nullptr) {
        return false;
    }
    VkPhysicalDeviceIDProperties idProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &idProps;
    getProps2(physicalDevice, &props);
    std::copy(std::begin(idProps.deviceUUID), std::end(idProps.deviceUUID), outUuid);
    return true;
}

VkVideoEncoderHandleOwnership ToExtOwnership(HandleOwnership ownership)
{
    return (ownership == HandleOwnership::Borrow)
               ? VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_BORROW
               : VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_TRANSFER;
}

VkVideoEncoderExternalHandleType ToExtHandleType(ExternalHandleType type)
{
    switch (type) {
    case ExternalHandleType::NoHandle:          return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_NONE;
    case ExternalHandleType::OpaqueFd:      return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD;
    case ExternalHandleType::DmaBuf:        return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF;
    case ExternalHandleType::OpaqueWin32:   return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32;
    case ExternalHandleType::D3D11Texture:  return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_D3D11_TEXTURE;
    case ExternalHandleType::VkImageHandle: return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    }
    return VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_NONE;
}

// The descriptor API speaks two error currencies; this interface speaks one. A VkResult
// that has no closer meaning becomes InternalError rather than being dropped,
// and the detail string keeps the distinction visible in a log.
Result FromVkResult(VkResult result, const char* detail)
{
    switch (result) {
    case VK_SUCCESS:                     return Result();
    case VK_NOT_READY:                   return Result(ResultCode::NotReady, detail);
    case VK_TIMEOUT:                     return Result(ResultCode::Timeout, detail);
    case VK_ERROR_FORMAT_NOT_SUPPORTED:  return Result(ResultCode::UnsupportedFormat, detail);
    case VK_ERROR_FEATURE_NOT_PRESENT:   return Result(ResultCode::UnsupportedFeature, detail);
    case VK_ERROR_DEVICE_LOST:           return Result(ResultCode::DeviceLost, detail);
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:  return Result(ResultCode::OutOfResources, detail);
    case VK_ERROR_INITIALIZATION_FAILED: return Result(ResultCode::InternalError, detail);
    default:                             return Result(ResultCode::InternalError, detail);
    }
}

Result FromExtStatusCode(VkVideoEncoderStatusCode code, const char* detail)
{
    if (code == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        return Result();
    }
    switch (code) {
    case VK_VIDEO_ENCODER_STATUS_NOT_READY:
        return Result(ResultCode::NotReady, detail);
    case VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED:
    case VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED:
    case VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED:
        return Result(ResultCode::UnsupportedFormat, detail);
    case VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED:
    case VK_VIDEO_ENCODER_STATUS_ERROR_EXTENSION_MISSING:
    case VK_VIDEO_ENCODER_STATUS_ERROR_SHARING_MODE_UNSUPPORTED:
    case VK_VIDEO_ENCODER_STATUS_ERROR_API_VERSION_UNSUPPORTED:
        return Result(ResultCode::UnsupportedFeature, detail);
    case VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN:
    case VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID:
    case VK_VIDEO_ENCODER_STATUS_ERROR_ALLOCATION_SIZE_INVALID:
    case VK_VIDEO_ENCODER_STATUS_ERROR_EXTENT_INVALID:
    case VK_VIDEO_ENCODER_STATUS_ERROR_USAGE_INSUFFICIENT:
    case VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN:
    case VK_VIDEO_ENCODER_STATUS_ERROR_DEVICE_MISMATCH:
    case VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED:
        return Result(ResultCode::InvalidArgument, detail);
    case VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT:
        return Result(ResultCode::OutOfResources, detail);
    case VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED:
        return Result(ResultCode::NotConfigured, detail);
    default:
        return Result(ResultCode::InternalError, detail);
    }
}

//=============================================================================
// EXTERNAL IMAGE
//=============================================================================

// Fill the descriptor the implementation expects. The OS handle travels
// separately because the descriptor API takes it as its own argument.
void ToExtImageDescriptor(const ExternalImage&                   image,
                          VkVideoEncoderExternalImageDescriptor& out,
                          uint64_t&                              outOsHandle)
{
    out = VkVideoEncoderExternalImageDescriptor();
    out.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    out.pNext = nullptr;

    out.handleType    = ToExtHandleType(image.handleType);
    out.format        = image.format;
    out.width         = image.width;
    out.height        = image.height;
    out.imageType     = VK_IMAGE_TYPE_2D;
    out.mipLevels     = 1;
    out.arrayLayers   = 1;
    out.samples       = VK_SAMPLE_COUNT_1_BIT;
    out.tiling        = image.tiling;
    out.imageUsage    = image.usage;
    out.imageFlags    = image.createFlags;
    out.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    out.residency     = ToExtResidency(image.residency, image.handleType);
    out.ownership     = ToExtOwnership(image.ownership);
    out.defaultLayout = image.layout;
    out.colorModel    = ToExtColorModel(image.colorModel);
    out.existingImage = image.existingImage;

    out.hasDrmFormatModifier = image.hasDrmFormatModifier ? VK_TRUE : VK_FALSE;
    out.drmFormatModifier    = image.drmFormatModifier;

    out.planeCount = image.planeCount;
    for (uint32_t i = 0; i < image.planeCount && i < VK_VIDEO_ENCODER_MAX_PLANES; ++i) {
        out.planeLayouts[i].offset   = image.planeLayouts[i].offset;
        out.planeLayouts[i].rowPitch = image.planeLayouts[i].rowPitch;
        out.planeLayouts[i].size     = image.planeLayouts[i].size;
    }

    std::copy(std::begin(image.deviceUUID), std::end(image.deviceUUID),
              std::begin(out.deviceUUID));
    std::copy(std::begin(image.driverUUID), std::end(image.driverUUID),
              std::begin(out.driverUUID));
    std::copy(std::begin(image.deviceLUID), std::end(image.deviceLUID),
              std::begin(out.deviceLUID));
    out.deviceLUIDValid = image.deviceLuidValid ? VK_TRUE : VK_FALSE;

    out.allocationSize  = image.allocationSize;
    out.memoryTypeBits  = image.memoryTypeBits;
    out.memoryTypeIndex = image.memoryTypeIndex;

    switch (image.handleType) {
    case ExternalHandleType::OpaqueFd:
    case ExternalHandleType::DmaBuf:
        outOsHandle = static_cast<uint64_t>(static_cast<uint32_t>(image.fd));
        break;
    case ExternalHandleType::OpaqueWin32:
    case ExternalHandleType::D3D11Texture:
        outOsHandle = reinterpret_cast<uint64_t>(image.win32Handle);
        break;
    default:
        outOsHandle = 0;
        break;
    }
}

} // namespace

//=============================================================================
// CAPABILITIES
//=============================================================================

class CapsImpl final : public IEncoderCaps {
public:
    CapsImpl(VulkanVideoEncoderContext* ctx, uint32_t deviceIndex)
        : m_ctx(ctx), m_deviceIndex(deviceIndex) { }

    void* QueryInterface(std::string_view id) override
    {
        if (id == IEncoderCaps::kId) {
            return static_cast<IEncoderCaps*>(this);
        }
        return nullptr;
    }

    ArrayView<Codec> Codecs() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_codecs.empty()) {
            for (Codec codec : {Codec::H264, Codec::H265, Codec::AV1}) {
                VkVideoEncoderCapabilities caps{};
                caps.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES;
                if (VkEncGetEncodeCapabilities(m_ctx, m_deviceIndex, ToVkCodec(codec),
                                               VK_VIDEO_ENCODER_PROFILE_DEFAULT,
                                               &caps) == VK_SUCCESS) {
                    m_codecs.push_back(codec);
                }
            }
        }
        return ArrayView<Codec>(m_codecs);
    }

    ArrayView<Profile> Profiles(Codec codec) const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_profiles.find(codec);
        if (it == m_profiles.end()) {
            std::vector<Profile> supported;
            static const Profile kAll[] = {
                Profile::H264Baseline, Profile::H264Main, Profile::H264High, Profile::H264High10,
                Profile::H265Main, Profile::H265Main10, Profile::H265MainStillPicture,
                Profile::H265Rext,
                Profile::AV1Main, Profile::AV1High, Profile::AV1Professional,
            };
            for (Profile profile : kAll) {
                if (!ProfileMatchesCodec(codec, profile)) {
                    continue;
                }
                VkVideoEncoderCapabilities caps{};
                caps.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES;
                if (VkEncGetEncodeCapabilities(m_ctx, m_deviceIndex, ToVkCodec(codec),
                                               ToExtProfile(profile), &caps) == VK_SUCCESS) {
                    supported.push_back(profile);
                }
            }
            it = m_profiles.emplace(codec, std::move(supported)).first;
        }
        return ArrayView<Profile>(it->second);
    }

    ArrayView<InputFormat> InputFormats(Profile profile) const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_inputFormats.find(profile);
        if (it != m_inputFormats.end()) {
            return ArrayView<InputFormat>(it->second);
        }

        const VkVideoCodecOperationFlagBitsKHR codec = CodecForProfile(profile);
        std::vector<InputFormat> formats;

        uint32_t count = 0;
        if (VkEncEnumerateInputFormats(m_ctx, m_deviceIndex, codec, ToExtProfile(profile),
                                       &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkVideoEncoderInputFormatProperties> props(count);
            if (VkEncEnumerateInputFormats(m_ctx, m_deviceIndex, codec, ToExtProfile(profile),
                                           &count, props.data()) == VK_SUCCESS) {
                formats.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    InputFormat f;
                    f.format     = props[i].format;
                    f.colorModel = ColorModel::FromFormat;
                    f.isOptimal  = (props[i].optimality ==
                                    VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL);
                    // A format the encoder can take unchanged needs no
                    // conversion; one whose session format differs is reached
                    // by a copy, or by the filter when the samples themselves
                    // must change.
                    f.path = (props[i].format == props[i].encodeFormat)
                                 ? InputPath::Direct
                                 : InputPath::Copy;
                    formats.push_back(f);
                }
            }
        }
        it = m_inputFormats.emplace(profile, std::move(formats)).first;
        return ArrayView<InputFormat>(it->second);
    }

    ArrayView<uint64_t> DrmModifiers(Profile profile, VkFormat format) const override
    {
        (void)profile;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_modifiers.find(format);
        if (it != m_modifiers.end()) {
            return ArrayView<uint64_t>(it->second);
        }

        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        std::vector<uint64_t> modifiers;
        uint32_t count = 0;
        if (VkEncEnumerateDrmModifiers(m_ctx, m_deviceIndex, format, usage,
                                       &count, nullptr) == VK_SUCCESS && count > 0) {
            modifiers.resize(count);
            if (VkEncEnumerateDrmModifiers(m_ctx, m_deviceIndex, format, usage,
                                           &count, modifiers.data()) != VK_SUCCESS) {
                modifiers.clear();
            }
        }
        it = m_modifiers.emplace(format, std::move(modifiers)).first;
        return ArrayView<uint64_t>(it->second);
    }

    RateControlCaps RateControl(Profile profile) const override
    {
        RateControlCaps out;
        VkVideoEncoderCapabilities caps{};
        if (!GetCaps(profile, caps)) {
            return out;
        }
        out.supportsCbr = (caps.supportedRateControlModes &
                           VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) != 0;
        out.supportsVbr = (caps.supportedRateControlModes &
                           VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) != 0;
        out.supportsConstantQp = (caps.supportedRateControlModes &
                                  VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) != 0;
        out.maxQualityLevels = caps.maxQualityLevels;
        out.maxBitrate       = caps.maxBitrate;
        return out;
    }

    VkExtent2D MinCodedExtent(Profile profile) const override
    {
        VkVideoEncoderCapabilities caps{};
        if (!GetCaps(profile, caps)) {
            return VkExtent2D{0, 0};
        }
        return caps.minCodedExtent;
    }

    VkExtent2D MaxCodedExtent(Profile profile) const override
    {
        VkVideoEncoderCapabilities caps{};
        if (!GetCaps(profile, caps)) {
            return VkExtent2D{0, 0};
        }
        return caps.maxCodedExtent;
    }

    bool Supports(Feature feature) const override
    {
        VkVideoEncoderCapabilities caps{};
        switch (feature) {
        case Feature::IntraRefresh:
            return GetCaps(Profile::Default, caps) && caps.supportsIntraRefresh;
        case Feature::HdrMetadata:
            // Carried by H.265 and AV1; H.264 has no such SEI.
            return true;
        case Feature::DmaBufImport:
        case Feature::ExternalSemaphores:
        case Feature::RegisteredResources:
        case Feature::Reconfigure:
            return true;
        case Feature::BFrames:
            return GetCaps(Profile::Default, caps) && caps.maxDpbSlots > 2;
        }
        return false;
    }

private:
    VkVideoCodecOperationFlagBitsKHR CodecForProfile(Profile profile) const
    {
        for (Codec codec : {Codec::H264, Codec::H265, Codec::AV1}) {
            if (profile != Profile::Default && ProfileMatchesCodec(codec, profile)) {
                return ToVkCodec(codec);
            }
        }
        return VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    }

    bool GetCaps(Profile profile, VkVideoEncoderCapabilities& caps) const
    {
        caps.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES;
        caps.pNext = nullptr;
        return VkEncGetEncodeCapabilities(m_ctx, m_deviceIndex, CodecForProfile(profile),
                                          ToExtProfile(profile), &caps) == VK_SUCCESS;
    }

    VulkanVideoEncoderContext* m_ctx;
    uint32_t                   m_deviceIndex;

    mutable std::mutex                                m_mutex;
    mutable std::vector<Codec>                        m_codecs;
    mutable std::map<Codec, std::vector<Profile>>     m_profiles;
    mutable std::map<Profile, std::vector<InputFormat>> m_inputFormats;
    mutable std::map<VkFormat, std::vector<uint64_t>> m_modifiers;
};

//=============================================================================
// DEVICE BINDING
//=============================================================================

class DeviceBindingImpl final : public IDeviceBinding {
public:
    static constexpr std::string_view kId = IDeviceBinding::kId;

    DeviceBindingImpl(const PlatformCreateInfo& info) : m_info(info) { }

    void* QueryInterface(std::string_view id) override
    {
        if (id == IDeviceBinding::kId) {
            return static_cast<IDeviceBinding*>(this);
        }
        return nullptr;
    }

    // Once a session exists it owns the authoritative handles, including the
    // ones a library-created device only has after initialisation.
    void BindSession(const VkSharedBaseObj<VulkanVideoEncoderExt>& encoder)
    {
        m_encoder = encoder;
    }

    VkInstance Instance() const override
    {
        return m_encoder ? m_encoder->GetVkInstance() : m_info.instance;
    }
    VkPhysicalDevice PhysicalDevice() const override
    {
        return m_encoder ? m_encoder->GetVkPhysicalDevice() : m_info.physicalDevice;
    }
    VkDevice Device() const override
    {
        return m_encoder ? m_encoder->GetVkDevice() : m_info.device;
    }
    uint32_t EncodeQueueFamilyIndex() const override  { return m_info.encodeQueueFamilyIndex; }
    uint32_t ComputeQueueFamilyIndex() const override { return m_info.computeQueueFamilyIndex; }

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const override
    {
        return m_encoder ? m_encoder->GetVkGetInstanceProcAddr() : nullptr;
    }

    bool DeviceUuid(uint8_t outUuid[VK_UUID_SIZE]) const override
    {
        return VkEncQueryDeviceUuid(Instance(), PhysicalDevice(),
                                    GetInstanceProcAddr(), outUuid);
    }

private:
    PlatformCreateInfo                     m_info;
    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;
};

//=============================================================================
// CONFIGURATION
//=============================================================================

// How a session reaches the descriptor-API configuration behind an
// IEncoderConfig.
//
// This library and Chromium both build with -fno-rtti, so dynamic_cast is not
// available to check that a caller handed back a configuration this library
// made. QueryInterface already is a typed downcast that needs no RTTI, so it
// serves here too and there is only one mechanism to understand. The id is not
// in the public header: it is a private channel between two classes in this
// file, reached through the public mechanism.
class IConfigAccess : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.internal.IConfigAccess/1";

    virtual const VkVideoEncoderConfig& ExtConfig() const = 0;
    virtual bool RequiresImportExtensions() const = 0;
    virtual bool                        HasHdrMetadata() const = 0;
    virtual const HdrMetadata&          GetHdrMetadata() const = 0;
};

// Accumulates a VkVideoEncoderConfig, and answers the questions the caller
// would otherwise have to answer for itself.
//
// The HDR role is offered only for codecs that can carry the metadata, so an
// H.264 caller learns that HDR is unavailable from a null Query rather than
// from a refused session.
class ConfigImpl final : public IEncoderConfig,
                         public IHdrMetadataConfig,
                         public IConfigAccess {
public:
    ConfigImpl(Codec codec, Profile profile, const PlatformCreateInfo& platform)
        : m_codec(codec), m_profile(profile)
    {
        // The struct carries default member initialisers, so it is
        // value-initialised and then overwritten field by field. Blanking it
        // would replace each documented default with whatever zero means for
        // that field.
        m_config = VkVideoEncoderConfig();
        m_config.pNext = nullptr;
        m_config.codec   = ToVkCodec(codec);
        m_config.profile = ToExtProfile(profile);

        m_config.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
        m_config.tuningMode      = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;
        m_config.inputColorModel = VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT;

        // An embedding host has nowhere to write and no reason to. A file is
        // written only once SetOutputPath names one.
        m_config.disableFileOutput = VK_TRUE;
        m_config.outputPath        = nullptr;

        // externalInstance and externalPhysicalDevice ARE DELIBERATELY NOT SET
        // from |platform|, even when the caller supplied them. Every session
        // this interface creates is created ON the platform's context, and a
        // context already carries the instance and the physical device it was
        // built against. Naming them a second time here is a contradiction the
        // encoder refuses outright ("the context already supplies both"),
        // which would make an adopted-instance platform unable to create any
        // session at all.
        //
        // externalDevice and the queue families below are a different case and
        // do pass through: a context never creates a VkDevice, so a caller
        // supplying one is adding something the context does not have.
        m_config.externalDevice                  = platform.device;
        m_config.externalEncodeQueueFamilyIndex  = platform.encodeQueueFamilyIndex;
        m_config.externalComputeQueueFamilyIndex = platform.computeQueueFamilyIndex;

        m_config.deviceId    = platform.deviceIndex;
        m_config.validate    = platform.enableValidation ? VK_TRUE : VK_FALSE;
        m_config.silenceStdio = platform.silenceStdio ? VK_TRUE : VK_FALSE;
        m_config.verbose     = VK_FALSE;
        if (platform.gpuUuidValid) {
            std::copy(std::begin(platform.gpuUuid), std::end(platform.gpuUuid),
                      std::begin(m_config.gpuUUID));
        }

        // The three bitstream code points default to "unspecified" (2) rather
        // than to a guess about the caller's content.
        m_config.colourPrimaries         = 2;
        m_config.transferCharacteristics = 2;
        m_config.matrixCoefficients      = 2;
        // The input's transfer function defaults to 0, which is a different
        // statement: it claims nothing, leaving the input in whatever
        // transferCharacteristics ends up declaring. 2 here would be a claim
        // of "unspecified" that conflicts with every bitstream declaring a
        // real transfer function.
        m_config.inputTransferCharacteristics = 0;
    }

    void* QueryInterface(std::string_view id) override
    {
        if (id == IEncoderConfig::kId) {
            return static_cast<IEncoderConfig*>(this);
        }
        if (id == IHdrMetadataConfig::kId && CodecCarriesHdrMetadata()) {
            return static_cast<IHdrMetadataConfig*>(this);
        }
        if (id == IConfigAccess::kId) {
            return static_cast<IConfigAccess*>(this);
        }
        return nullptr;
    }

    IEncoderConfig& SetCodedExtent(uint32_t width, uint32_t height) override
    {
        m_config.encodeWidth  = width;
        m_config.encodeHeight = height;
        if (m_config.inputWidth == 0) {
            m_config.inputWidth  = width;
            m_config.inputHeight = height;
        }
        return *this;
    }

    IEncoderConfig& SetInputExtent(uint32_t width, uint32_t height) override
    {
        m_config.inputWidth  = width;
        m_config.inputHeight = height;
        return *this;
    }

    IEncoderConfig& SetFrameRate(uint32_t numerator, uint32_t denominator) override
    {
        m_config.frameRateNum = numerator;
        // A zero denominator with a real numerator means whole frames per
        // second, which is what a host that tracks only a rate passes.
        m_config.frameRateDen =
            (denominator == 0 && numerator != 0) ? 1 : denominator;
        return *this;
    }

    IEncoderConfig& SetInputFormat(VkFormat format, ColorModel colorModel) override
    {
        m_config.inputFormat     = format;
        m_config.inputColorModel = ToExtColorModel(colorModel);
        return *this;
    }

    IEncoderConfig& SetRateControl(const RateControl& rc) override
    {
        m_config.rateControlMode = ToExtRateControl(rc.mode);
        m_config.averageBitrate  = rc.averageBitrate;
        m_config.maxBitrate      = rc.maxBitrate;
        m_config.vbvBufferSize   = rc.vbvBufferSize;
        m_config.constQpI        = rc.constQpIntra;
        m_config.constQpP        = rc.constQpPredicted;
        m_config.constQpB        = rc.constQpBidirectional;
        m_config.minQp           = rc.minQp;
        m_config.maxQp           = rc.maxQp;
        m_config.qualityLevel    = rc.qualityLevel;
        m_config.tuningMode      = ToExtTuning(rc.tuning);
        return *this;
    }

    IEncoderConfig& SetGop(const GopStructure& gop) override
    {
        m_config.gopLength          = gop.gopLength;
        m_config.idrPeriod          = gop.idrPeriod;
        m_config.consecutiveBFrames = gop.consecutiveBFrames;
        m_config.closedGop          = gop.closedGop ? VK_TRUE : VK_FALSE;
        return *this;
    }

    IEncoderConfig& SetColourInfo(const ColourInfo& colour) override
    {
        m_config.colourPrimaries              = colour.colourPrimaries;
        m_config.transferCharacteristics      = colour.transferCharacteristics;
        m_config.matrixCoefficients           = colour.matrixCoefficients;
        m_config.videoFullRange               = colour.fullRange ? VK_TRUE : VK_FALSE;
        m_config.inputTransferCharacteristics = colour.inputTransferCharacteristics;
        return *this;
    }

    IEncoderConfig& RequireImportExtensions(bool require) override
    {
        m_requireImportExtensions = require;
        return *this;
    }

    IEncoderConfig& SetOutputPath(const char* path) override
    {
        if (path != nullptr && path[0] != '\0') {
            m_outputPath = path;
            m_config.outputPath        = m_outputPath.c_str();
            m_config.disableFileOutput = VK_FALSE;
        } else {
            m_outputPath.clear();
            m_config.outputPath        = nullptr;
            m_config.disableFileOutput = VK_TRUE;
        }
        return *this;
    }

    Codec   GetCodec() const override   { return m_codec; }
    Profile GetProfile() const override { return m_profile; }

    // The rule, in one place: a copy engine moves samples the encoder can
    // already read; a compute filter runs only when the samples themselves
    // must change. A caller asks instead of deciding.
    InputPath ResolveInputPath() const override
    {
        if (m_config.inputFormat == VK_FORMAT_UNDEFINED) {
            return InputPath::Copy;
        }
        const VkEncInputFormatClass cls =
            VkEncClassifyInput(m_config.inputFormat, m_config.inputColorModel);
        if (cls == VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER) {
            return InputPath::ComputeFilter;
        }
        if (cls == VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
            return InputPath::ComputeFilter;
        }
        // Encodable as-is: a copy still runs when the geometry or the tiling
        // differs, but no sample is rewritten.
        return InputPath::Copy;
    }

    VkFormat ResolveSessionFormat() const override
    {
        return m_config.inputFormat;
    }

    Result Validate() const override
    {
        if (m_config.encodeWidth == 0 || m_config.encodeHeight == 0) {
            return Result(ResultCode::InvalidArgument, "coded extent is unset");
        }
        if (m_config.inputFormat == VK_FORMAT_UNDEFINED) {
            return Result(ResultCode::InvalidArgument, "input format is unset");
        }
        // NO FRAME-RATE REQUIREMENT. The encoder has a default and coerces a
        // zero denominator to one, so refusing here would refuse a
        // configuration the encoder accepts -- and Validate exists to catch
        // what the encoder would refuse, not to invent rules of its own. A
        // check stricter than the implementation turns a working host into a
        // silent no-output.
        if (!ProfileMatchesCodec(m_codec, m_profile)) {
            return Result(ResultCode::UnsupportedProfile, "profile does not belong to this codec");
        }
        if (VkEncClassifyInput(m_config.inputFormat, m_config.inputColorModel) ==
            VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
            return Result(ResultCode::UnsupportedFormat,
                          "the encoder cannot take this input format on any path");
        }
        const bool constantQp =
            m_config.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
        if (!constantQp &&
            m_config.rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR &&
            m_config.averageBitrate == 0) {
            return Result(ResultCode::InvalidArgument, "a bitrate mode needs an average bitrate");
        }
        return Result();
    }

    void SetHdrMetadata(const HdrMetadata& metadata) override
    {
        m_hdr        = metadata;
        m_hasHdr     = true;
    }

    // Consumed by the session: the descriptor API takes HDR metadata as a
    // chained struct, and the chain is built only at that point so nothing
    // dangles on a configuration the caller keeps.
    const VkVideoEncoderConfig& ExtConfig() const override { return m_config; }
    bool RequiresImportExtensions() const override
    {
        return m_requireImportExtensions;
    }
    bool                        HasHdrMetadata() const override { return m_hasHdr; }

    const HdrMetadata&          GetHdrMetadata() const override { return m_hdr; }

private:
    bool CodecCarriesHdrMetadata() const
    {
        // H.264 defines no mastering-display or content-light SEI.
        return m_codec != Codec::H264;
    }

    bool                 m_requireImportExtensions = false;
    Codec                m_codec;
    Profile              m_profile;
    VkVideoEncoderConfig m_config;
    std::string          m_outputPath;
    HdrMetadata          m_hdr;
    bool                 m_hasHdr = false;
};

//=============================================================================
// SESSION
//=============================================================================

// One object implementing the session and every role it can serve. The roles
// are separate interfaces so a caller depends on the four methods it uses; a
// single implementation behind them is an implementation detail.
class SessionImpl final : public IEncoderSession,
                          public IFrameSubmitter,
                          public IBitstreamSource,
                          public ICompletionSignal,
                          public IResourceRegistry,
                          public IDeviceBinding,
                          public IDiagnostics,
                          public IShutdownDiagnostics,
                          public std::enable_shared_from_this<SessionImpl> {
public:
    SessionImpl(const VkSharedBaseObj<VulkanVideoEncoderExt>& encoder,
                const VkVideoEncoderConfig&                    config)
        : m_encoder(encoder), m_config(config) { }

    // Detach before the encoder reference goes.
    //
    // Dropping m_encoder is not the same as destroying the encoder: an
    // aliased role can still hold it, and its workers can still be draining.
    // Whatever survives holds a callback registration this session installed,
    // so the registration has to come down here rather than be left to the
    // encoder's own teardown.
    //
    // The detach is a quiesce point -- it returns only once any in-flight
    // invocation has returned -- and it hands the cookie back to
    // TrampolineRelease, which frees it and runs the client release. No lock
    // is held across it, for the reason SetCallback explains.
    ~SessionImpl() override
    {
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_closing = true;
        }
        if (m_encoder) {
            // A refused detach cannot be reported from here and cannot be
            // retried by abandoning the destructor. It is survivable rather
            // than silent: the cookie belongs to the encoder either way, so
            // refusing to detach leaks a registration, not a dangling one.
            (void)m_encoder->SetCompletionCallback(nullptr, nullptr, nullptr);
        }
    }

    void* QueryInterface(std::string_view id) override
    {
        if (id == IEncoderSession::kId)   return static_cast<IEncoderSession*>(this);
        if (id == IFrameSubmitter::kId)   return static_cast<IFrameSubmitter*>(this);
        if (id == IBitstreamSource::kId)  return static_cast<IBitstreamSource*>(this);
        if (id == ICompletionSignal::kId) return static_cast<ICompletionSignal*>(this);
        if (id == IResourceRegistry::kId) return static_cast<IResourceRegistry*>(this);
        if (id == IDeviceBinding::kId)    return static_cast<IDeviceBinding*>(this);
        if (id == IDiagnostics::kId)      return static_cast<IDiagnostics*>(this);
        if (id == IShutdownDiagnostics::kId)
            return static_cast<IShutdownDiagnostics*>(this);
        return nullptr;
    }

    //---- IEncoderSession ----------------------------------------------------

    // Apply what the session can carry and name what it cannot.
    //
    // Only rate control and frame rate can move mid-stream. Everything else is
    // settled in the sequence header written once, or in the input routing the
    // session was built around, so it needs a new session. The library holds
    // the configuration the session was created with and compares against it,
    // which is what lets a caller hand back either the object it built the
    // session from or a fresh one carrying only the change.
    //
    // A field left at its unset value means "leave this alone" rather than
    // "set this to zero" -- otherwise a caller moving only the frame rate
    // would have to restate the resolution, the format and the bitrate to
    // avoid being refused for changing them.
    Result Reconfigure(const Ref<IEncoderConfig>& config) override
    {
        if (!config) {
            return Fail(Result(ResultCode::InvalidArgument, "no configuration"));
        }
        Ref<IConfigAccess> access = Query<IConfigAccess>(config);
        if (!access) {
            return Fail(Result(ResultCode::InvalidArgument, "foreign configuration object"));
        }
        const VkVideoEncoderConfig& in = access->ExtConfig();

        std::lock_guard<std::mutex> lock(m_configMutex);

        if (const char* immutable = FirstImmutableChange(in)) {
            RecordDetail(immutable);
            return Fail(Result(ResultCode::UnsupportedFeature,
                               "this field cannot change without a new session"));
        }

        // Start from what the session is running, and overlay only what moves.
        VkVideoEncoderConfig next = m_config;
        next.pNext = nullptr;   // a chain is refused; the session already has its metadata

        if (in.averageBitrate != 0) {
            next.averageBitrate = in.averageBitrate;
        }
        // Zero here is coerced to the average rather than meaning "no cap", so
        // an unstated maximum carries forward instead of silently capping the
        // session at its own average.
        if (in.maxBitrate != 0) {
            next.maxBitrate = in.maxBitrate;
        }
        if (in.frameRateNum != 0) {
            next.frameRateNum = in.frameRateNum;
            next.frameRateDen = (in.frameRateDen != 0) ? in.frameRateDen : 1;
        }
        // A negative constant-QP member names no quantizer; zero is a valid
        // (lossless) one, so the two cannot be collapsed.
        if (in.constQpI >= 0) { next.constQpI = in.constQpI; }
        if (in.constQpP >= 0) { next.constQpP = in.constQpP; }
        if (in.constQpB >= 0) { next.constQpB = in.constQpB; }
        next.minQp = in.minQp;
        next.maxQp = in.maxQp;

        if (next.averageBitrate == 0 &&
            next.rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
            return Fail(Result(ResultCode::InvalidArgument,
                               "a rate-controlled session needs a non-zero average bitrate"));
        }

        const VkResult result = m_encoder->Reconfigure(next);
        if (result != VK_SUCCESS) {
            return Fail(FromVkResult(result, "the session refused this reconfiguration"));
        }
        m_config = next;
        return Result();
    }

    Result Drain() override
    {
        return Fail(FromVkResult(m_encoder->DrainPendingFrames(),
                                 "the drain did not complete"));
    }

    Result Finish() override
    {
        // The underlying Flush is the terminal one: it releases the encoder on
        // the way out, which is why Drain above is a different call and not a
        // parameter of this one.
        return Fail(FromVkResult(m_encoder->Flush(), "the stream could not be ended"));
    }

    Expected<uint32_t> AbandonAll() override
    {
        uint32_t abandoned = 0;
        const VkResult result = m_encoder->AbandonAllFrames(&abandoned);
        if (result != VK_SUCCESS) {
            return Fail(FromVkResult(result, "abandon failed"));
        }
        return abandoned;
    }

    //---- IFrameSubmitter ----------------------------------------------------

    Result SubmitFrame(const FrameSubmit& frame) override
    {
        // The descriptor API takes parallel arrays; this interface takes one array of
        // pairs, which is the shape that cannot go out of step.
        std::vector<VkSemaphore> waitSems, signalSems;
        std::vector<uint64_t>    waitVals, signalVals;
        Unpack(frame.waitSemaphores, waitSems, waitVals);
        Unpack(frame.signalSemaphores, signalSems, signalVals);

        if (frame.registeredImage != kNoResource) {
            return SubmitRegistered(frame, waitSems, waitVals, signalSems, signalVals);
        }

        // Same fence pair as the registered path: an inline image has the same
        // producer-ordering problem, and answering it on only one of the two
        // paths would make the choice of path change the semantics.
        VkVideoEncoderFrameFenceDescriptor fences;
        fences.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR;
        fences.pNext = nullptr;
        fences.acquireFenceFd  = frame.acquireFenceFd;
        fences.pReleaseFenceFd = frame.releaseFenceFd;
        const bool wantFences =
            (frame.acquireFenceFd >= 0) || (frame.releaseFenceFd != nullptr);

        VkVideoEncodeInputFrame in{};
        in.sType  = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME;
        in.pNext  = wantFences ? &fences : nullptr;
        in.image  = frame.image.existingImage;
        in.format = frame.image.format;
        in.width  = frame.image.width;
        in.height = frame.image.height;
        in.imageTiling  = frame.image.tiling;
        in.currentLayout = frame.image.layout;
        in.frameId  = frame.frameId;
        in.pts      = frame.pts;
        in.forceIDR = frame.forceIdr ? VK_TRUE : VK_FALSE;
        in.isLastFrame = frame.isLastFrame ? VK_TRUE : VK_FALSE;
        in.qpOverride  = LowerQpOverride(frame);

        in.waitSemaphoreCount   = static_cast<uint32_t>(waitSems.size());
        in.pWaitSemaphores      = waitSems.empty() ? nullptr : waitSems.data();
        in.pWaitSemaphoreValues = waitVals.empty() ? nullptr : waitVals.data();
        in.signalSemaphoreCount   = static_cast<uint32_t>(signalSems.size());
        in.pSignalSemaphores      = signalSems.empty() ? nullptr : signalSems.data();
        in.pSignalSemaphoreValues = signalVals.empty() ? nullptr : signalVals.data();

        const VkResult result = m_encoder->SubmitExternalFrame(in, nullptr);
        if (result == VK_SUCCESS) {
            ++m_framesSubmitted;
        }
        return Fail(FromVkResult(result, "frame submission failed"));
    }

    // A frame whose image was registered earlier. The registration already
    // holds the import, so this path costs no image creation -- which is the
    // reason a compositor registers at all.
    Result SubmitRegistered(const FrameSubmit&              frame,
                            const std::vector<VkSemaphore>& waitSems,
                            const std::vector<uint64_t>&    waitVals,
                            const std::vector<VkSemaphore>& signalSems,
                            const std::vector<uint64_t>&    signalVals)
    {
        // The fence pair is a chained structure on this API. It lives for the
        // duration of the call, which is all the library needs: it consumes
        // the acquire fd and writes the release fd synchronously.
        VkVideoEncoderFrameFenceDescriptor fences;
        fences.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR;
        fences.pNext = nullptr;
        fences.acquireFenceFd   = frame.acquireFenceFd;
        fences.pReleaseFenceFd  = frame.releaseFenceFd;
        const bool wantFences =
            (frame.acquireFenceFd >= 0) || (frame.releaseFenceFd != nullptr);

        VkVideoEncoderFrameSubmitInfo info;
        info.sType    = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.resource = static_cast<VkVideoEncoderResource>(frame.registeredImage);
        info.frameId  = frame.frameId;
        info.pts      = frame.pts;
        info.forceIDR = frame.forceIdr ? VK_TRUE : VK_FALSE;
        info.isLastFrame = frame.isLastFrame ? VK_TRUE : VK_FALSE;
        info.qpOverride = LowerQpOverride(frame);
        // UNDEFINED asks the encoder to use the layout declared at
        // registration, which is what a caller that has not moved the image
        // wants.
        info.currentLayout = frame.image.layout;
        info.pNext = wantFences ? &fences : nullptr;

        info.waitSemaphoreCount   = static_cast<uint32_t>(waitSems.size());
        info.pWaitSemaphores      = waitSems.empty() ? nullptr : waitSems.data();
        info.pWaitSemaphoreValues = waitVals.empty() ? nullptr : waitVals.data();
        info.signalSemaphoreCount   = static_cast<uint32_t>(signalSems.size());
        info.pSignalSemaphores      = signalSems.empty() ? nullptr : signalSems.data();
        info.pSignalSemaphoreValues = signalVals.empty() ? nullptr : signalVals.data();

        const VkVideoEncoderStatusCode code =
            m_encoder->SubmitRegisteredFrame(info, nullptr);
        if (code == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            ++m_framesSubmitted;
        }
        RecordCode("registered frame submission failed", code);
        return Fail(FromExtStatusCode(code, "registered frame submission failed"));
    }

    Result CancelFrame(uint64_t frameId) override
    {
        return Fail(FromVkResult(m_encoder->CancelFrame(frameId), "frame is not cancellable"));
    }

    FrameState GetFrameState(uint64_t frameId) const override
    {
        return FromExtFrameState(m_encoder->GetFrameStatus(frameId));
    }

    //---- IBitstreamSource ---------------------------------------------------

    Expected<EncodedFrame> AcquireNext() override
    {
        VkVideoEncodeResult out{};
        out.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT;
        const VkResult result = m_encoder->AcquireNextEncodedFrame(out);
        if (result != VK_SUCCESS) {
            return Fail(FromVkResult(result, "no encoded frame is ready"));
        }
        ++m_framesEncoded;
        return FromExtResult(out);
    }

    Expected<EncodedFrame> Acquire(uint64_t frameId) override
    {
        VkVideoEncodeResult out{};
        out.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT;
        const VkResult result = m_encoder->AcquireEncodedFrame(frameId, out);
        if (result != VK_SUCCESS) {
            return Fail(FromVkResult(result, "that frame is not ready"));
        }
        ++m_framesEncoded;
        return FromExtResult(out);
    }

    void Release(uint64_t frameId) override
    {
        m_encoder->ReleaseEncodedFrame(frameId);
    }

    //---- ICompletionSignal --------------------------------------------------

    Result SetCallback(CompletionCallback callback, void* userData,
                       UserDataRelease release) override
    {
        // NEVER HOLD m_callbackMutex ACROSS THE LIBRARY CALL. Installing or
        // clearing a callback is a quiesce point: it returns only once any
        // in-flight invocation has returned. If that invocation were waiting
        // on a lock held here, the two would wait on each other. The
        // trampolines below take no adapter lock at all, which is what keeps
        // that true even while a replacement is in progress.
        //
        // Keep this session and its encoder alive for the whole method. A
        // client release runs synchronously inside the lower call, and a
        // release handler that drops the caller's last Ref would otherwise
        // destroy the object executing this line.
        const Ref<SessionImpl>                       self = shared_from_this();
        const VkSharedBaseObj<VulkanVideoEncoderExt> encoder = m_encoder;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_closing) {
                return Fail(Result(ResultCode::NotConfigured,
                                   "the session is shutting down"));
            }
        }

        if (callback == nullptr) {
            // Detach at the library. Once it returns, no trampoline can be
            // running and the cookie has been handed back to
            // TrampolineRelease -- which is what makes the promise that a
            // caller may destroy whatever userData referenced once this
            // returns.
            //
            // A refused detach leaves the OLD registration installed. Do not
            // record the session as callback-free in that case: the caller
            // must be told its cookie is still reachable.
            const Result detached = Fail(FromVkResult(
                encoder->SetCompletionCallback(nullptr, nullptr, nullptr),
                "the completion callback could not be cleared"));
            if (detached.ok()) {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                m_hasCallback = false;
            }
            return detached;
        }

        // The cookie is a state object the lower layer owns, never this
        // session. Allocate it before the detach so a failed allocation
        // cannot leave the session with no callback at all.
        CallbackState* state = new (std::nothrow)
            CallbackState(callback, userData, release);
        if (state == nullptr) {
            return Fail(Result(ResultCode::OutOfResources,
                               "the completion callback state could not be "
                               "allocated"));
        }

        // Replacing one callback with another goes through a detach, so no
        // in-flight invocation can straddle the change and deliver the new
        // cookie to the old callback. The library sees the same trampoline
        // pointer either way and would not quiesce on its own.
        bool hadCallback = false;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            hadCallback = m_hasCallback;
        }
        if (hadCallback) {
            // A refused detach means the old registration is still live.
            // Installing over it would strand the old cookie, whose release
            // the encoder would then never call. Refuse, and destroy the
            // state this call allocated rather than the one still in use.
            const Result detached = Fail(FromVkResult(
                encoder->SetCompletionCallback(nullptr, nullptr, nullptr),
                "the previous completion callback could not be replaced"));
            if (!detached.ok()) {
                delete state;
                return detached;
            }
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_hasCallback = false;
        }

        // ALWAYS install the release thunk, even when the client supplied no
        // release of its own: the thunk is what frees the state object. With
        // nullptr here the encoder would drop the cookie on the floor at
        // detach and every SetCallback would leak one.
        const Result installed = Fail(FromVkResult(
            encoder->SetCompletionCallback(&SessionImpl::TrampolineCallback,
                                           state,
                                           &SessionImpl::TrampolineRelease),
            "the completion callback could not be installed"));
        if (!installed.ok()) {
            // Ownership transfers only on success. A refused installation
            // never reached the encoder, so nothing else will free this.
            delete state;
            return installed;
        }
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_hasCallback = true;
        }
        return installed;
    }

    uint64_t CompletedCount() const override
    {
        return m_encoder->GetCompletionCounter();
    }

    //---- IShutdownDiagnostics -----------------------------------------------

    ShutdownSnapshot Shutdown() const override
    {
        VkVideoEncoderShutdownInfo info{};
        m_encoder->GetShutdownInfo(&info);

        ShutdownSnapshot out;
        switch (info.disposition) {
        case VK_VIDEO_ENCODER_SHUTDOWN_IDLE:
            out.disposition = ShutdownDisposition::Idle;
            break;
        case VK_VIDEO_ENCODER_SHUTDOWN_LOST_DEVICE_RETIRED:
            out.disposition = ShutdownDisposition::LostDeviceRetired;
            break;
        default:
            out.disposition = ShutdownDisposition::Unproven;
            break;
        }
        out.firstError = FromVkResult(info.firstError, "").code();
        out.workersJoined      = (info.workersJoined != VK_FALSE);
        out.callbackDetached   = (info.callbackDetached != VK_FALSE);
        out.deviceLostObserved = (info.deviceLostObserved != VK_FALSE);
        out.complete           = (info.shutdownComplete != VK_FALSE);
        return out;
    }

    VkSemaphore CompletionSemaphore() const override
    {
        return m_encoder->GetCompletionSemaphore();
    }

    Expected<int> ExportCompletionHandle() override
    {
        uint64_t handle = 0;
        const VkVideoEncoderStatusCode code = m_encoder->GetCompletionEventHandle(&handle);
        if (code != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            return Fail(FromExtStatusCode(code, "no completion handle is available"));
        }
        // The lower layer's handle is BORROWED: the encoder signals through it
        // and closes it at teardown. The interface hands the caller a close
        // obligation, so each export must be a handle of its own. Returning the
        // borrowed one makes a caller that honours the documented contract close
        // the encoder's live event -- after which completions are written to a
        // descriptor the process has since reused, and teardown closes it twice.
        const uint64_t duplicate = vkenc::OsCompletionEventDuplicate(handle);
        if (duplicate == vkenc::kOsCompletionEventNone) {
            return Fail(Result(ResultCode::OutOfResources,
                               "the completion handle could not be duplicated for export"));
        }
        return static_cast<int>(duplicate);
    }

    //---- IResourceRegistry --------------------------------------------------

    Expected<ResourceId> RegisterImage(const ExternalImage& image,
                                       bool* outHandleConsumed) override
    {
        if (outHandleConsumed != nullptr) {
            *outHandleConsumed = false;
        }
        VkVideoEncoderExternalImageDescriptor desc;
        uint64_t osHandle = 0;
        ToExtImageDescriptor(image, desc, osHandle);

        // The echo is written on every return, so it is read on every return.
        VkVideoEncoderStatus echo;
        echo.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
        echo.pNext = nullptr;

        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode code =
            m_encoder->RegisterImageResource(desc, osHandle, &resource, &echo);

        // Written before the refusal below, not after it: the echo's whole
        // purpose is to be true on a path that returns no value.
        if (outHandleConsumed != nullptr) {
            *outHandleConsumed = (echo.handlesConsumed != VK_FALSE);
        }

        if (code != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            RecordCode("image could not be registered", code);
            return Fail(FromExtStatusCode(code, "image could not be registered"));
        }
        return static_cast<ResourceId>(resource);
    }

    Result UnregisterImage(ResourceId id) override
    {
        if (id == kNoResource) {
            return Fail(Result(ResultCode::InvalidArgument, "no such registered image"));
        }
        return Fail(FromExtStatusCode(
            m_encoder->UnregisterImageResource(static_cast<VkVideoEncoderResource>(id)),
            "image could not be unregistered"));
    }

    Expected<InputPath> QueryImageSupport(const ExternalImage& image) override
    {
        VkVideoEncoderExternalImageDescriptor desc;
        uint64_t osHandle = 0;
        ToExtImageDescriptor(image, desc, osHandle);

        VkVideoEncoderImageSupport support{};
        support.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT;
        const VkVideoEncoderStatusCode code =
            m_encoder->QueryImageSupport(desc, &support);
        if (code != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            return Fail(FromExtStatusCode(code, "this image cannot be imported"));
        }
        // TWO ANSWERS, AND BOTH MEAN NO. The call reports whether it could be
        // answered; the reply reports whether the image is usable. A query
        // that was answered "no" returns SUCCESS, so reading only the status
        // turns a refusal into an acceptance.
        if (support.supported != VK_TRUE) {
            return Fail(Result(ResultCode::UnsupportedFormat,
                               "the encoder cannot take this image"));
        }
        const VkEncInputFormatClass cls =
            VkEncClassifyInput(image.format, ToExtColorModel(image.colorModel));
        return (cls == VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT) ? InputPath::Copy
                                                             : InputPath::ComputeFilter;
    }

    Expected<ResourceId> RegisterSemaphore(const ExternalSemaphore& semaphore) override
    {
        VkVideoEncoderSemaphoreDescriptor desc;
        desc.sType      = VK_VIDEO_ENCODER_STRUCTURE_TYPE_SEMAPHORE_DESCRIPTOR;
        desc.pNext      = nullptr;
        desc.handleType = ToExtHandleType(semaphore.handleType);
        // Timeline only: a binary semaphore cannot report a completion to a
        // consumer that was not already waiting when it was signalled.
        desc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        desc.ownership     = ToExtOwnership(semaphore.ownership);

        uint64_t osHandle = 0;
        switch (semaphore.handleType) {
        case ExternalHandleType::OpaqueFd:
            osHandle = static_cast<uint64_t>(static_cast<uint32_t>(semaphore.fd));
            break;
        case ExternalHandleType::OpaqueWin32:
            osHandle = reinterpret_cast<uint64_t>(semaphore.win32Handle);
            break;
        default:
            return Fail(Result(ResultCode::InvalidArgument,
                               "a semaphore needs an importable handle type"));
        }

        VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
        const VkVideoEncoderStatusCode code =
            m_encoder->RegisterSemaphore(desc, osHandle, &resource, nullptr);
        if (code != VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            return Fail(FromExtStatusCode(code, "semaphore could not be registered"));
        }
        return static_cast<ResourceId>(resource);
    }

    Result UnregisterSemaphore(ResourceId id) override
    {
        if (id == kNoResource) {
            return Fail(Result(ResultCode::InvalidArgument, "no such registered semaphore"));
        }
        return Fail(FromExtStatusCode(
            m_encoder->UnregisterSemaphore(static_cast<VkVideoEncoderResource>(id)),
            "semaphore could not be unregistered"));
    }

    //---- IDeviceBinding -----------------------------------------------------
    //
    // A session runs on a device, so it can say which. The platform answers
    // the same question for a caller that has not created a session yet; a
    // caller that has one should not have to keep the platform alive, or hold
    // a second object, to ask what device its own session is on.

    VkInstance Instance() const override { return m_encoder->GetVkInstance(); }
    VkPhysicalDevice PhysicalDevice() const override
    {
        return m_encoder->GetVkPhysicalDevice();
    }
    VkDevice Device() const override { return m_encoder->GetVkDevice(); }

    // The queue families are the session's own business and it does not
    // publish them; a caller that supplied them already knows what it gave.
    uint32_t EncodeQueueFamilyIndex() const override  { return UINT32_MAX; }
    uint32_t ComputeQueueFamilyIndex() const override { return UINT32_MAX; }

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const override
    {
        return m_encoder->GetVkGetInstanceProcAddr();
    }

    bool DeviceUuid(uint8_t outUuid[VK_UUID_SIZE]) const override
    {
        return VkEncQueryDeviceUuid(m_encoder->GetVkInstance(),
                                    m_encoder->GetVkPhysicalDevice(),
                                    m_encoder->GetVkGetInstanceProcAddr(),
                                    outUuid);
    }

    //---- IDiagnostics -------------------------------------------------------

    RuntimeInfo GetRuntimeInfo() const override
    {
        RuntimeInfo out;
        VkVideoEncoderRuntimeInfo info{};
        info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_RUNTIME_INFO;
        if (m_encoder->GetRuntimeInfo(&info) != VK_SUCCESS) {
            return out;
        }
        // The source is a fixed-width field that need not be terminated, so
        // one byte is reserved for the terminator this side guarantees.
        const size_t room = sizeof(out.implementationName) - 1;
        std::copy(info.implementationName, info.implementationName + room,
                  out.implementationName);
        out.implementationName[room] = '\0';
        out.isHardwareAccelerated    = info.isHardwareAccelerated != VK_FALSE;
        out.supportsNativeHandle     = info.supportsNativeHandle != VK_FALSE;
        out.trustedRateController    = info.trustedRateController != VK_FALSE;
        out.supportsSimulcast        = info.supportsSimulcast != VK_FALSE;
        out.supportsFrameSizeChange  = info.supportsFrameSizeChange != VK_FALSE;
        out.reportsAverageQp         = info.reportsAverageQp != VK_FALSE;
        out.applyAlignmentToAllSimulcastLayers =
            info.applyAlignmentToAllSimulcastLayers != VK_FALSE;
        out.resolutionAlignmentWidth =
            info.requestedResolutionAlignmentWidth ? info.requestedResolutionAlignmentWidth : 1;
        out.resolutionAlignmentHeight =
            info.requestedResolutionAlignmentHeight ? info.requestedResolutionAlignmentHeight : 1;
        return out;
    }

    const char* LastErrorDetail() const override
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError.c_str();
    }

    Expected<CompletionStats> GetCompletionStats() const override
    {
        VkVideoEncoderCompletionInfo info;
        info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;

        // The diagnostics ride in on the chain here so the caller receives one
        // value rather than having to know that two structures exist.
        VkVideoEncoderDiagnosticInfo diagnostics;
        diagnostics.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_DIAGNOSTIC_INFO;
        diagnostics.pNext = nullptr;
        diagnostics.diagnosticCount = 0;
        diagnostics.lastDiagnostic[0] = '\0';
        info.pNext = &diagnostics;

        const VkResult result = m_encoder->GetCompletionInfo(&info);
        if (result != VK_SUCCESS) {
            return Fail(FromVkResult(result, "the session has no completion accounting"));
        }

        CompletionStats stats;
        stats.completionCounter = info.completionCounter;
        stats.framesTimedOut    = info.framesTimedOut;
        stats.lateCaptures      = info.lateCaptures;
        stats.framesCancelled   = info.framesCancelled;
        stats.framesPending     = info.framesPending;
        stats.framesReady       = info.framesReady;
        stats.framesAcquired    = info.framesAcquired;
        stats.diagnosticCount   = diagnostics.diagnosticCount;

        const size_t room = sizeof(stats.lastDiagnostic) - 1;
        const size_t take = (VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS - 1 < room)
                                ? VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS - 1
                                : room;
        std::copy(diagnostics.lastDiagnostic, diagnostics.lastDiagnostic + take,
                  stats.lastDiagnostic);
        stats.lastDiagnostic[take] = '\0';
        return stats;
    }

    uint64_t FramesSubmitted() const override { return m_framesSubmitted; }
    uint64_t FramesEncoded() const override   { return m_framesEncoded; }

private:
    // The first field that is both stated by the caller and different from
    // what the session runs, or null when nothing immutable moves. An unset
    // field is not a change: it is the absence of one.
    const char* FirstImmutableChange(const VkVideoEncoderConfig& in) const
    {
        if (in.codec != m_config.codec)     { return "codec"; }
        if (in.profile != m_config.profile) { return "profile"; }
        if (in.encodeWidth != 0 && in.encodeWidth != m_config.encodeWidth) {
            return "coded width";
        }
        if (in.encodeHeight != 0 && in.encodeHeight != m_config.encodeHeight) {
            return "coded height";
        }
        if (in.inputFormat != VK_FORMAT_UNDEFINED &&
            in.inputFormat != m_config.inputFormat) {
            return "input format";
        }
        if (in.rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR &&
            in.rateControlMode != m_config.rateControlMode) {
            return "rate control mode";
        }
        // 2 is "unspecified" in every one of these code points, and is what an
        // untouched configuration carries.
        if (in.colourPrimaries != 2 && in.colourPrimaries != m_config.colourPrimaries) {
            return "colour primaries";
        }
        if (in.transferCharacteristics != 2 &&
            in.transferCharacteristics != m_config.transferCharacteristics) {
            return "transfer characteristics";
        }
        if (in.matrixCoefficients != 2 &&
            in.matrixCoefficients != m_config.matrixCoefficients) {
            return "matrix coefficients";
        }
        if (in.gopLength != 0 && in.gopLength != m_config.gopLength) {
            return "GOP length";
        }
        if (in.qualityLevel != 0 && in.qualityLevel != m_config.qualityLevel) {
            return "quality level";
        }
        return nullptr;
    }

    void RecordDetail(const char* what) const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::string("cannot change ") + what + " without a new session";
        m_lastErrorPinned = true;
    }

    // The encoder hands back the cookie it was given, which is a
    // CallbackState and never this session. The lower layer owns it from a
    // successful SetCompletionCallback until it hands it here, exactly once,
    // after any in-flight invocation has returned -- on replacement, on
    // detach, or at encoder destruction. This is the only place it is freed.
    static void TrampolineRelease(void* userData) VK_VIDEO_ENCODER_CB_NOEXCEPT
    {
        CallbackState* state = static_cast<CallbackState*>(userData);
        if (state == nullptr) {
            return;
        }
        if (state->release != nullptr) {
            state->release(state->userData);
        }
        delete state;
    }

    // Reads immutable state through a pointer the encoder guarantees is live
    // for the duration of the call. It deliberately touches no session member
    // and takes no session lock: a session destroyed while the encoder is
    // still draining must not turn a completion into a use-after-free, and a
    // trampoline that waited on an adapter lock would deadlock against the
    // quiesce point in SetCallback.
    static void TrampolineCallback(uint64_t frameId, void* userData)
        VK_VIDEO_ENCODER_CB_NOEXCEPT
    {
        const CallbackState* state = static_cast<const CallbackState*>(userData);
        if (state != nullptr && state->callback != nullptr) {
            state->callback(frameId, state->userData);
        }
    }

    // The ONE place the public flag/value pair becomes a lower-layer field.
    //
    // -1 is the lower layer's "no override". Zero is not: it is a legitimate
    // constant QP a caller may ask for deliberately, which is exactly why the
    // public contract carries a separate hasQpOverride flag. The two arms
    // disagreed here -- the inline arm sent 0 for an unset override, so a
    // caller that never asked for one had every inline frame encoded at QP 0
    // while the same submission through the registered arm behaved correctly.
    //
    // The sentinel is documented at this boundary and nowhere else: the
    // public default stays zero, and an intentional QP of zero still reaches
    // the encoder as zero.
    static int32_t LowerQpOverride(const FrameSubmit& frame)
    {
        return frame.hasQpOverride ? frame.qpOverride : -1;
    }

    static void Unpack(const ArrayView<SemaphoreWait>& in,
                       std::vector<VkSemaphore>&       sems,
                       std::vector<uint64_t>&          values)
    {
        sems.reserve(in.size());
        values.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            sems.push_back(in[i].semaphore);
            values.push_back(in[i].value);
        }
    }

    static EncodedFrame FromExtResult(const VkVideoEncodeResult& in)
    {
        EncodedFrame out;
        out.frameId = in.frameId;
        out.pts     = in.pts;
        out.dts     = in.dts;
        out.bitstream = ArrayView<uint8_t>(in.pBitstreamData, in.bitstreamSize);
        out.pictureType = FromExtPictureType(in.pictureType);
        out.isIdr       = in.isIDR != VK_FALSE;
        out.temporalLayerId = in.temporalLayerId;
        // A deadline drop is delivered like any other frame and carries no
        // bitstream, so the outcome travels with it rather than being inferred
        // from an empty view.
        out.outcome = FromVkResult(in.status, "").code();
        return out;
    }

    // The status code a Result cannot carry -- its detail is a literal --
    // recorded where IDiagnostics can hand it back. This is the split the
    // interface promises: literal in Result, runtime context in IDiagnostics.
    void RecordCode(const char* what, VkVideoEncoderStatusCode code) const
    {
        if (code == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::string(what) + " (encoder status " +
                      std::to_string(static_cast<int>(code)) + ")";
        m_lastErrorPinned = true;
    }

    // Every failing Result also lands in LastErrorDetail, which is where the
    // runtime context Result deliberately omits can be recovered from.
    const Result& Fail(const Result& status) const
    {
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            if (m_lastErrorPinned) {
                // RecordCode already wrote the fuller message for this
                // failure; do not overwrite it with the literal.
                m_lastErrorPinned = false;
            } else {
                m_lastError = status.detail();
            }
        }
        return status;
    }

    VkSharedBaseObj<VulkanVideoEncoderExt> m_encoder;

    // What the session is running. Reconfigure overlays onto this rather than
    // replacing it, so a caller may hand back a configuration carrying only
    // the change.
    std::mutex           m_configMutex;
    VkVideoEncoderConfig m_config;

    mutable std::mutex  m_errorMutex;
    mutable std::string m_lastError;
    mutable bool        m_lastErrorPinned = false;

    // What the lower layer holds while a callback is installed. Immutable
    // once constructed, so the trampolines read it without a lock; owned by
    // the encoder, so it outlives this session whenever the encoder does.
    struct CallbackState {
        CallbackState(CompletionCallback cb, void* data, UserDataRelease rel)
            : callback(cb), userData(data), release(rel) { }

        const CompletionCallback callback;
        void* const              userData;
        const UserDataRelease    release;
    };

    // Guards only the two bookkeeping bits below. The callback itself is not
    // reachable from here on purpose -- see TrampolineCallback.
    std::mutex m_callbackMutex;
    bool       m_hasCallback = false;
    bool       m_closing     = false;

    uint64_t m_framesSubmitted = 0;
    uint64_t m_framesEncoded   = 0;
};

//=============================================================================
// PLATFORM
//=============================================================================

class PlatformImpl final : public IEncoderPlatform,
                          public IPlatformLifetime {
public:
    PlatformImpl(const PlatformCreateInfo&                        info,
                 const VkSharedBaseObj<VulkanVideoEncoderContext>& context)
        : m_info(info)
        , m_context(context)
        , m_caps(std::make_shared<CapsImpl>(context.get(),
                                            info.deviceIndex < 0 ? 0u
                                                : static_cast<uint32_t>(info.deviceIndex)))
        , m_binding(std::make_shared<DeviceBindingImpl>(info))
    { }

    void* QueryInterface(std::string_view id) override
    {
        if (id == IEncoderPlatform::kId) {
            return static_cast<IEncoderPlatform*>(this);
        }
        if (id == IPlatformLifetime::kId) {
            return static_cast<IPlatformLifetime*>(this);
        }
        return nullptr;
    }

    // Releases the process-wide floor reference, not this platform: the
    // instance being destroyed is shared by every platform built for this
    // device, which is why retiring it is a request a caller has to make
    // rather than something a single platform's destructor may do.
    uint32_t Retire() override { return VkEncRetireOwnContexts(); }

    Ref<IEncoderCaps>   Caps() const override          { return m_caps; }
    Ref<IDeviceBinding> DeviceBinding() const override { return m_binding; }

    Expected<Ref<IEncoderConfig>> CreateConfig(Codec codec, Profile profile) override
    {
        if (!ProfileMatchesCodec(codec, profile)) {
            return Result(ResultCode::UnsupportedProfile,
                          "profile does not belong to this codec");
        }
        Ref<IEncoderConfig> config = std::make_shared<ConfigImpl>(codec, profile, m_info);
        return config;
    }

    Expected<Ref<IEncoderSession>> CreateSession(const Ref<IEncoderConfig>& config) override
    {
        if (!config) {
            return Result(ResultCode::InvalidArgument, "no configuration");
        }
        Ref<IConfigAccess> access = Query<IConfigAccess>(config);
        if (!access) {
            return Result(ResultCode::InvalidArgument, "foreign configuration object");
        }
        if (Result valid = config->Validate(); !valid) {
            return valid;
        }

        // ON THE PLATFORM'S CONTEXT, not beside it. The platform IS the device
        // scope: it holds the context that answered every capability question
        // above, and a session created independently would build a SECOND
        // Vulkan instance and device of its own.
        //
        // In a process that already owns Vulkan -- a compositor, a renderer,
        // anything that draws what it encodes -- that second device is not
        // merely wasteful. It is another instance in a process that has one,
        // and initialisation fails.
        VkSharedBaseObj<VulkanVideoEncoderExt> encoder;
        const uint32_t deviceIndex =
            (m_info.deviceIndex < 0) ? 0u : static_cast<uint32_t>(m_info.deviceIndex);
        if (VkResult result =
                CreateVulkanVideoEncoderExtOnContext(m_context, deviceIndex, encoder);
            result != VK_SUCCESS) {
            return FromVkResult(result, "the encoder could not be created");
        }

        // The HDR chain is built here and lives only for the duration of the
        // call, so nothing dangles on a configuration the caller keeps.
        VkVideoEncoderConfig extConfig = access->ExtConfig();
        // deviceId stays at its "no selection" value: the context already
        // chose the physical device, and a session built on one refuses a
        // second selector rather than letting a config field silently outrank
        // the device whose capabilities the caller queried.
        VkVideoEncoderHdrMetadataInfo hdr;
        if (access->HasHdrMetadata()) {
            const HdrMetadata& src = access->GetHdrMetadata();
            hdr = VkVideoEncoderHdrMetadataInfo();
            hdr.masteringDisplayPresent = src.masteringDisplayPresent ? VK_TRUE : VK_FALSE;
            for (int i = 0; i < 3; ++i) {
                hdr.displayPrimaryX[i] = src.displayPrimaryX[i];
                hdr.displayPrimaryY[i] = src.displayPrimaryY[i];
            }
            hdr.whitePointX  = src.whitePointX;
            hdr.whitePointY  = src.whitePointY;
            hdr.maxDisplayMasteringLuminance = src.maxLuminance;
            hdr.minDisplayMasteringLuminance = src.minLuminance;
            hdr.contentLightLevelPresent = src.contentLightLevelPresent ? VK_TRUE : VK_FALSE;
            hdr.maxContentLightLevel      = src.maxContentLightLevel;
            hdr.maxFrameAverageLightLevel = src.maxFrameAverageLightLevel;
            extConfig.pNext = &hdr;
        }

        // Chained onto a copy that lives only for this call: the retained
        // baseline a reconfiguration overlays onto must carry no chain.
        VkVideoEncoderValidationInfo validation;
        if (access->RequiresImportExtensions()) {
            validation = VkVideoEncoderValidationInfo();
            validation.flags = VK_VIDEO_ENCODER_VALIDATE_EXTENSIONS_BIT;
            validation.pNext = extConfig.pNext;
            extConfig.pNext  = &validation;
        }

        if (VkResult result = encoder->InitializeExt(extConfig); result != VK_SUCCESS) {
            return FromVkResult(result, "the encoder refused this configuration");
        }

        m_binding->BindSession(encoder);

        // The session keeps the configuration it was created with, so
        // Reconfigure can overlay onto it. The chain is NOT kept: it points at
        // a local that dies with this call, and the metadata it carried is
        // already written into the session's parameter sets.
        VkVideoEncoderConfig retained = extConfig;
        retained.pNext = nullptr;
        Ref<IEncoderSession> session = std::make_shared<SessionImpl>(encoder, retained);
        return session;
    }

private:
    PlatformCreateInfo                        m_info;
    VkSharedBaseObj<VulkanVideoEncoderContext> m_context;
    Ref<CapsImpl>                             m_caps;
    Ref<DeviceBindingImpl>                    m_binding;
};

} // namespace enc
} // namespace video
} // namespace vk

//=============================================================================
// FORMAT CLASSIFICATION
//
// The four questions a host asks about a format before it allocates -- can you
// take this, by which route, what does the filter need of my image, and what
// will the session run at -- answered once, from the library's own routing
// tables rather than from a copy of them in the caller.
//=============================================================================

extern "C" VK_ENC_EXPORT
VkResult VkEncClassifyFormat(VkFormat                       format,
                             vk::video::enc::ColorModel     colorModel,
                             vk::video::enc::FormatRouting* outRouting)
{
    using namespace vk::video::enc;

    if (outRouting == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *outRouting = FormatRouting();

    const VkVideoEncoderColorModel model = ToExtColorModel(colorModel);
    const VkEncInputFormatClass    cls   = VkEncClassifyInput(format, model);

    if (cls == VK_ENC_INPUT_FORMAT_UNSUPPORTED) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    outRouting->supported = true;

    if (cls == VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT) {
        // The encoder reads these samples as they are. A copy may still move
        // them, but nothing rewrites one.
        outRouting->path          = InputPath::Copy;
        outRouting->filterAccess  = FilterAccess::NoFilter;
        outRouting->sessionFormat = format;
        return VK_SUCCESS;
    }

    outRouting->path = InputPath::ComputeFilter;

    // How the filter must reach the source. A planar source is addressed one
    // plane at a time; a single-plane source -- packed YCbCr or RGB -- is read
    // as one storage image. The distinction decides what usage and what view
    // formats the caller's image needs, which is why it is answered here
    // rather than inferred by each host from a format list of its own.
    outRouting->filterAccess = (VkEncInputFormatPlaneCount(format) > 1)
                                   ? FilterAccess::PlaneStorage
                                   : FilterAccess::StorageRead;

    // Without a device list the conversion target is knowable only where it
    // does not depend on one. It does for RGB, whose session takes the
    // device's first advertised encode-source format.
    outRouting->sessionFormat =
        VkEncConversionTargetFormat(format, nullptr, 0);

    return VK_SUCCESS;
}

//=============================================================================
// PLATFORM CREATION -- THE SECOND EXPORTED SYMBOL
//=============================================================================

extern "C" VK_ENC_EXPORT
VkResult VkEncCreatePlatform(const vk::video::enc::PlatformCreateInfo& createInfo,
                             vk::video::enc::Ref<vk::video::enc::IEncoderPlatform>& outPlatform)
{
    using namespace vk::video::enc;

    VkVideoEncoderContextCreateInfo contextInfo{};
    contextInfo.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONTEXT_CREATE_INFO;
    contextInfo.pNext = nullptr;

    // Adopting the caller's instance is what keeps the encoder on the device
    // that already owns the images; creating one is for the standalone tools.
    const bool adopt = (createInfo.instance != VK_NULL_HANDLE);
    contextInfo.mode = adopt ? VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT
                             : VK_VIDEO_ENCODER_CONTEXT_MODE_OWN;
    contextInfo.adoptInstance       = createInfo.instance;
    contextInfo.adoptPhysicalDevice = createInfo.physicalDevice;
    contextInfo.silenceStdio        = createInfo.silenceStdio ? VK_TRUE : VK_FALSE;
    if (createInfo.gpuUuidValid) {
        std::copy(std::begin(createInfo.gpuUuid), std::end(createInfo.gpuUuid),
                  std::begin(contextInfo.gpuUUID));
    }

    VkSharedBaseObj<VulkanVideoEncoderContext> context;
    const VkResult result = CreateVulkanVideoEncoderContext(&contextInfo, context);
    if (result != VK_SUCCESS) {
        return result;
    }

    outPlatform = std::make_shared<PlatformImpl>(createInfo, context);
    return VK_SUCCESS;
}
