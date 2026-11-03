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

#ifndef _VULKAN_VIDEO_ENCODER_H_
#define _VULKAN_VIDEO_ENCODER_H_

// The encoder interface: abstract, reference-counted C++ roles.
//
// THE LANGUAGE FLOOR IS C++17, and it is a hard constraint on this file rather
// than a preference. Consumers include this header; they do not compile the
// implementation, which targets C++20. Nothing here may use std::span,
// std::expected, designated initialisers, concepts or coroutines. A
// syntax-only compile at -std=c++17 is part of the test suite.
//
// THE CALLER NEVER LAYS OUT AN OBJECT THIS LIBRARY OWNS. Every interface below
// is allocated by the library and reached through a Ref. That is what allows a
// later version to add behaviour: a new interface derives from an old one, and
// callers built against the old one neither recompile nor notice. Types the
// CALLER constructs -- PlatformCreateInfo, RateControl, FrameSubmit -- are
// plain aggregates and may never gain a field, because their layout is pinned
// by every caller that compiles against them. Extension happens on interfaces.
//
// A FEATURE THE IMPLEMENTATION DOES NOT PROVIDE IS A NULL Ref, not an error
// returned from a call. Query<I>() is both the capability test and the way to
// reach the capability, so a caller checks once at setup instead of at every
// call site.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "vulkan_interfaces.h"

#if defined(VK_VIDEO_ENCODER_SHAREDLIB)
#  if defined(_WIN32)
#    if defined(VK_VIDEO_ENCODER_IMPLEMENTATION)
#      define VK_ENC_EXPORT __declspec(dllexport)
#    else
#      define VK_ENC_EXPORT __declspec(dllimport)
#    endif
#  else
#    if defined(VK_VIDEO_ENCODER_IMPLEMENTATION)
#      define VK_ENC_EXPORT __attribute__((visibility("default")))
#    else
#      define VK_ENC_EXPORT
#    endif
#  endif
#else
#  define VK_ENC_EXPORT
#endif

namespace vk {
namespace video {
namespace enc {

//=============================================================================
// STATUS
//=============================================================================

// NOTHING IN THIS HEADER MAY BE NAMED Success, None, Status, Bool, Always,
// Complex, Convex, Above, Below, KeyPress OR CursorShape. Xlib defines every
// one of them as an object-like macro, so a consumer building with
// VK_USE_PLATFORM_XLIB_KHR -- which is every browser and every compositor on
// Linux -- would have them substituted before this header is even parsed. A
// namespace does not help against the preprocessor.
//
// Undefining them here is not an option: it would silently break the
// consumer's own use of Xlib. So the names are simply not used, which is why
// the status type is Result and its code type is ResultCode.
enum class ResultCode : int32_t {
    Ok = 0,

    // The caller can fix these.
    InvalidArgument,
    NotConfigured,
    AlreadyConfigured,
    UnsupportedCodec,
    UnsupportedProfile,
    UnsupportedFormat,
    UnsupportedFeature,
    OutOfRange,

    // Try again, or wait.
    NotReady,
    Timeout,
    WouldBlock,
    OutOfResources,

    // The session is finished.
    DeviceLost,
    InternalError,
};

// A code and a pointer to a string literal the library owns: 16 bytes,
// trivially copyable, no allocation. SubmitFrame returns one of these per
// frame, so it must not touch the heap.
//
// detail() is a literal, never a formatted message.
class Result {
public:
    Result() : m_code(ResultCode::Ok), m_detail("") { }
    Result(ResultCode code, const char* detail = "")
        : m_code(code), m_detail(detail ? detail : "") { }

    bool ok() const { return m_code == ResultCode::Ok; }
    explicit operator bool() const { return ok(); }

    ResultCode      code()   const { return m_code; }
    const char* detail() const { return m_detail; }

    bool operator==(ResultCode c) const { return m_code == c; }
    bool operator!=(ResultCode c) const { return m_code != c; }

private:
    ResultCode      m_code;
    const char* m_detail;
};

// A T, or the Result explaining why there is no T. std::expected is C++23, so
// this is permanent rather than a placeholder.
//
// Reading value() when the Result is an error is a caller bug; the value is
// default-constructed in that case rather than undefined, so a missed check
// misbehaves predictably instead of corrupting memory.
template <class T>
class Expected {
public:
    Expected(const T& value) : m_value(value) { }
    Expected(T&& value) : m_value(std::move(value)) { }
    Expected(const Result& status) : m_value(), m_status(status) { }

    bool ok() const { return m_status.ok(); }
    explicit operator bool() const { return ok(); }

    const T& value() const { return m_value; }
    T&       value()       { return m_value; }
    const T& operator*() const { return m_value; }
    T&       operator*()       { return m_value; }
    const T* operator->() const { return &m_value; }
    T*       operator->()       { return &m_value; }

    const Result& status() const { return m_status; }

private:
    T      m_value;
    Result m_status;
};

//=============================================================================
// ARRAY VIEW
//=============================================================================

// A borrowed, contiguous, read-only view: std::span without the C++20. The
// storage belongs to the library and stays valid until the object that
// returned it is released, which is what lets the enumerators return a view
// instead of making every caller run the count-then-fill dance twice.
template <class T>
class ArrayView {
public:
    ArrayView() : m_data(nullptr), m_size(0) { }
    ArrayView(const T* data, size_t size) : m_data(data), m_size(size) { }
    ArrayView(const std::vector<T>& v) : m_data(v.data()), m_size(v.size()) { }

    const T* data()  const { return m_data; }
    size_t   size()  const { return m_size; }
    bool     empty() const { return m_size == 0; }

    const T* begin() const { return m_data; }
    const T* end()   const { return m_data + m_size; }
    const T& operator[](size_t i) const { return m_data[i]; }

private:
    const T* m_data;
    size_t   m_size;
};

//=============================================================================
// OBJECT MODEL
//=============================================================================

template <class T> using Ref = std::shared_ptr<T>;

// The base of every library-owned object. It has exactly one virtual besides
// the destructor, and that one never grows: all extension happens by deriving
// new interfaces and answering their id here.
class IObject {
public:
    virtual ~IObject() = default;

    // Returns a pointer to the named interface, or nullptr if this object does
    // not implement it. Callers use Query<I>() rather than calling this.
    virtual void* QueryInterface(std::string_view id) = 0;

protected:
    IObject() = default;
};

// Reach a role on an object. Returns null when the implementation does not
// provide it, which is the capability query.
//
// The returned Ref shares the owner's reference count (the aliasing
// constructor), so holding a role keeps the object that vends it alive. A
// caller may drop the session Ref and keep only the roles it uses.
template <class I, class From>
Ref<I> Query(const Ref<From>& obj)
{
    if (!obj) {
        return Ref<I>();
    }
    void* iface = obj->QueryInterface(I::kId);
    if (iface == nullptr) {
        return Ref<I>();
    }
    return Ref<I>(obj, static_cast<I*>(iface));
}

//=============================================================================
// VOCABULARY
//=============================================================================

enum class Codec : uint32_t {
    H264 = 0,
    H265,
    AV1,
};

// The named profiles the library maps onto codec-specific profile_idc values.
// Default lets the library choose from the codec and the input geometry.
enum class Profile : uint32_t {
    Default = 0,
    H264Baseline, H264Main, H264High, H264High10,
    H265Main, H265Main10, H265MainStillPicture, H265Rext,
    AV1Main, AV1High, AV1Professional,
};

// How to read the samples in the caller's input image. FromFormat asks the
// library to decide from the VkFormat alone, which is right whenever the
// format is unambiguous.
enum class ColorModel : uint32_t {
    FromFormat = 0,
    Rgb,
    YCbCr,
};

enum class PictureType : uint32_t {
    Intra = 0,   // includes IDR and intra-refresh
    Predicted,
    Bidirectional,
};

enum class FrameState : uint32_t {
    Unknown = 0,   // never submitted, or already released
    Pending,       // submitted, not yet encoded
    Ready,         // an Acquire will deliver it
    Acquired,      // delivered, awaiting Release
};

enum class RateControlMode : uint32_t {
    Default = 0,
    Disabled,      // constant QP
    Cbr,
    Vbr,
};

enum class TuningMode : uint32_t {
    Default = 0,
    HighQuality,
    LowLatency,
    UltraLowLatency,
    Lossless,
};

// How the library obtains an input image the caller already owns. OpaqueFd
// and DmaBuf are imported from ExternalImage::fd; every other value names an
// image already on the session's device, in ExternalImage::existingImage.
enum class ExternalHandleType : uint32_t {
    NoHandle = 0,
    OpaqueFd,
    DmaBuf,
    OpaqueWin32,
    D3D11Texture,
    VkImageHandle,   // an image already resident on the shared VkDevice
};

// What InputPath the configuration resolves to. The library decides this from
// the input format, the colour model and the image layout; the caller asks
// rather than deriving it, so the rule has one implementation.
enum class InputPath : uint32_t {
    // The encoder reads the caller's image directly. No intermediate.
    Direct = 0,
    // A transfer-queue copy into an encodable image. The preferred path
    // whenever the samples are already in an encodable format and layout.
    Copy,
    // A compute shader converts the samples. Used only when the format, the
    // colour model or the plane layout cannot be resolved by a copy.
    ComputeFilter,
};

// What the compute filter needs of the caller's own image, when a format
// reaches the encoder through it. A host allocating the buffer has to know
// this BEFORE it allocates, and the answer is a property of the format rather
// than of any device or session.
enum class FilterAccess : uint32_t {
    // No filter runs; nothing extra is required of the image.
    NoFilter = 0,
    // The source is planar and the filter addresses its planes individually,
    // so the image needs per-plane storage views -- which in turn needs
    // MUTABLE_FORMAT and a format list naming the plane formats.
    PlaneStorage,
    // The source is a single plane, packed or RGB, and the filter reads it as
    // a storage image, so the image needs STORAGE usage.
    StorageRead,
};

// How one input format reaches the encoder. Answered without a device,
// because a host chooses its buffer format before it has one.
struct FormatRouting {
    bool supported = false;

    InputPath    path         = InputPath::Copy;
    FilterAccess filterAccess = FilterAccess::NoFilter;

    // What the session runs at. Equal to the input format on the direct path;
    // the conversion target when a filter runs. VK_FORMAT_UNDEFINED when the
    // answer needs a device, which is the RGB case: an RGB session takes the
    // device's first advertised encode-source format.
    VkFormat sessionFormat = VK_FORMAT_UNDEFINED;
};

// Optional behaviour a caller may need to know about before committing to a
// configuration. Supports() answers for the device the platform is bound to.
enum class Feature : uint32_t {
    IntraRefresh = 0,
    HdrMetadata,
    Reconfigure,
    BFrames,
};

//=============================================================================
// VALUE TYPES
//
// Constructed by the caller, so their layout is pinned by every caller that
// compiles. They may never gain a field. When one needs to grow, a new
// interface version takes a new type.
//=============================================================================

// Adopt the caller's Vulkan objects. A null device asks the library to create
// its own, which is what the standalone demos do; a browser or a compositor
// always supplies one, because the encoder must run on the device that already
// owns the images.
struct PlatformCreateInfo {
    VkInstance       instance                = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice          = VK_NULL_HANDLE;
    VkDevice         device                  = VK_NULL_HANDLE;
    uint32_t         encodeQueueFamilyIndex  = UINT32_MAX;
    uint32_t         computeQueueFamilyIndex = UINT32_MAX;

    // Used only when device is null: pick a device by index, or by UUID when
    // gpuUuidValid is set. A UUID is the stable choice across reboots.
    int32_t  deviceIndex  = -1;
    uint8_t  gpuUuid[VK_UUID_SIZE] = {};
    bool     gpuUuidValid = false;

    // Enable the Vulkan validation layers on a library-created instance. No
    // effect when the caller supplies its own instance.
    bool enableValidation = false;

    // Keep the library off stdout and stderr. A sandboxed host has no usable
    // stdio, and a library that writes to it anyway is a crash or a leak of
    // whatever the host had bound to those descriptors.
    //
    // THE EFFECT IS PROCESS-WIDE AND ONE-WAY. Silence holds while any live
    // platform, session or internal query is requesting it, and ends only when
    // the last of them is gone. Two callers cannot each choose: leaving this
    // false does not make the library audible while another owner needs it
    // quiet. Setting it true silences output the other owner would have seen.
    //
    // It covers the library's gated diagnostics. It is not a redirection of
    // the process's file descriptors and it does not relax a sandbox.
    bool silenceStdio = false;
};

struct RateControl {
    RateControlMode mode = RateControlMode::Default;

    uint32_t averageBitrate = 0;   // bits/sec
    uint32_t maxBitrate     = 0;   // bits/sec, VBR only
    uint32_t vbvBufferSize  = 0;   // bits; 0 asks the library to derive one

    // Used when mode is Disabled. Ignored otherwise.
    int32_t constQpIntra         = 0;
    int32_t constQpPredicted     = 0;
    int32_t constQpBidirectional = 0;

    // Clamps applied in every mode. Left at zero, the library uses the range
    // the device advertises.
    int32_t minQp = 0;
    int32_t maxQp = 0;

    uint32_t   qualityLevel = 0;   // 0 asks for the driver default
    TuningMode tuning       = TuningMode::Default;
};

struct GopStructure {
    // Frames per GOP; 0 leaves the length to the device's preferred value.
    uint32_t gopLength          = 0;
    // Frames between IDRs; 0 leaves the period to the device's preferred value. On a device
    // that reports no preference, only the first frame of the stream is an IDR.
    uint32_t idrPeriod          = 0;
    uint32_t consecutiveBFrames = 0;
    bool     closedGop          = false;
};

// How the caller's own samples are coded, in the code points of the video
// standards (ITU-T H.273). The library writes these into the bitstream and
// uses them to decide whether a conversion is needed.
struct ColourInfo {
    uint8_t colourPrimaries         = 2;   // 2 = unspecified
    uint8_t transferCharacteristics = 2;
    uint8_t matrixCoefficients      = 2;
    bool    fullRange               = false;

    // The transfer function the caller's input samples already carry.
    //
    // 0 is the only value that asserts nothing, and it is the default: the
    // input is taken to be in transferCharacteristics already. Every other
    // value is a positive claim, checked against transferCharacteristics and
    // refused when the two disagree -- this library converts the colour MODEL
    // and implements no transfer function, so it cannot reconcile them. Note
    // that 2 ("unspecified") is such a claim, not an absence: it is a value
    // the bitstream fields above may legitimately carry, but here it would
    // conflict with any bitstream that declares a real transfer function.
    uint8_t inputTransferCharacteristics = 0;
};

// SMPTE ST 2086 mastering display and content light level, in the units the
// standard codes and a producer already holds: chromaticity scaled by 50000,
// luminance by 10000. AV1 codes the same quantities in a different fixed-point
// format and primary order; the library converts, and the caller supplies one
// spelling only.
//
// All zeros is a claim, not an absence -- it says the mastering display is
// black -- so each payload has its own presence flag and they are written
// independently.
struct HdrMetadata {
    bool     masteringDisplayPresent = false;
    uint16_t displayPrimaryX[3] = {};   // ST 2086 order: 0 green, 1 blue, 2 red
    uint16_t displayPrimaryY[3] = {};
    uint16_t whitePointX = 0;
    uint16_t whitePointY = 0;
    uint32_t maxLuminance = 0;
    uint32_t minLuminance = 0;

    bool     contentLightLevelPresent = false;
    uint16_t maxContentLightLevel     = 0;
    uint16_t maxFrameAverageLightLevel = 0;
};

struct PlaneLayout {
    uint64_t offset   = 0;
    uint64_t rowPitch = 0;
    uint64_t size     = 0;
};

enum { kMaxPlanes = 4 };

// Who owns the fd of an fd-backed ExternalImage once SubmitFrame returns.
enum class HandleOwnership : uint32_t {
    // The library takes the fd. It is closed on every refusal, NotReady
    // included, and on success it belongs to the imported memory. The caller
    // never closes it.
    Transfer = 0,
    // The fd stays the caller's: the library imports a duplicate and never
    // closes the caller's fd.
    Borrow,
};

// Where an image lives relative to the encoder's device, which decides
// whether a queue-family ownership transfer is needed on acquire.
enum class Residency : uint32_t {
    // Inferred from the handle type. Right for a dma-buf or an OS handle,
    // where the library can tell.
    Auto = 0,
    // Allocated on the encoder's own device. No ownership transfer.
    Local,
    // Owned by VK_QUEUE_FAMILY_FOREIGN_EXT; acquired before use.
    Foreign,
};

// An image the caller already owns, in one of two forms.
//
// AN IMAGE ON THE SESSION'S DEVICE (handleType VkImageHandle, or any type
// other than OpaqueFd and DmaBuf). SubmitFrame reads existingImage -- a
// VkImage on the device the session runs on (IDeviceBinding::Device()) --
// and its format, width, height, tiling and layout. The encoder reads that
// image directly, or copies it into an image it can encode when the tiling,
// the extent or the layout calls for that. No OS handle is taken or closed.
//
// AN fd-BACKED IMAGE (handleType OpaqueFd or DmaBuf). SubmitFrame imports fd
// onto the session's device, as an image created exactly as described here,
// and encodes the frame from it; existingImage is not read. The description
// is the exporter's own and nothing in it is guessed:
//   * format, width, height, tiling, usage and createFlags are the
//     VkImageCreateInfo the exporter used. usage must include TRANSFER_SRC
//     for a LINEAR image, and TRANSFER_SRC or VIDEO_ENCODE_SRC otherwise.
//   * A DRM format modifier is stated with hasDrmFormatModifier,
//     drmFormatModifier, tiling VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and
//     one planeLayouts entry (offset, rowPitch) per memory plane of the
//     modifier; only a DmaBuf carries one.
//   * allocationSize is the exporter's allocation size; memoryTypeIndex
//     (OpaqueFd) is the exporter's memory type, and memoryTypeBits narrows
//     the choice when the index is not known.
//   * A non-zero deviceUUID, or a valid deviceLUID, must name the session's
//     device; a non-zero driverUUID must name its driver on OpaqueFd.
//   * layout is the layout the exporter left the memory in, and residency
//     says whether the encoder acquires it from VK_QUEUE_FAMILY_FOREIGN_EXT:
//     Auto reads as Foreign for an fd.
//   * ownership decides the fd's fate; see HandleOwnership. An ownership
//     value outside that enumeration is refused with the fd left open.
// A description the device cannot import is refused with a Result naming
// what is wrong, and the refusal follows ownership like any other.
//
// THE IMPORT IS PER FRAME. Each SubmitFrame imports its own image, which
// lives exactly as long as the encoder reads it: it is destroyed once the
// frame's input has been consumed -- the command buffers that read it have
// completed -- or when the session is released. An exporter reuses its
// allocation for a later frame once the frame's signal semaphores have
// signalled, and exports a new fd for every submission under Transfer.
struct ExternalImage {
    ExternalHandleType handleType = ExternalHandleType::NoHandle;

    // The fd when handleType is OpaqueFd or DmaBuf; the Vulkan image
    // otherwise.
    int      fd            = -1;
    void*    win32Handle   = nullptr;
    VkImage  existingImage = VK_NULL_HANDLE;

    HandleOwnership ownership = HandleOwnership::Transfer;

    VkFormat      format = VK_FORMAT_UNDEFINED;
    uint32_t      width  = 0;
    uint32_t      height = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ColorModel    colorModel = ColorModel::FromFormat;

    // The usage and flags the image was created with.
    VkImageUsageFlags  usage = 0;
    VkImageCreateFlags createFlags = 0;

    Residency residency = Residency::Auto;

    bool     hasDrmFormatModifier = false;
    uint64_t drmFormatModifier    = 0;

    uint32_t    planeCount = 0;
    PlaneLayout planeLayouts[kMaxPlanes] = {};

    uint64_t allocationSize = 0;
    uint32_t memoryTypeBits = 0;          // 0 when the caller does not know
    uint32_t memoryTypeIndex = UINT32_MAX;

    // Which device allocated the image. All-zero means "not stated".
    uint8_t deviceUUID[VK_UUID_SIZE] = {};
    uint8_t driverUUID[VK_UUID_SIZE] = {};
    uint8_t deviceLUID[VK_LUID_SIZE] = {};
    bool    deviceLuidValid = false;
};

// A wait/signal pair the library honours around the encode submission. A
// timeline value of zero means the semaphore is binary.
struct SemaphoreWait {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t    value     = 0;
};

// One frame handed to the encoder. frameId is the caller's, echoed back on
// every result and status query, and is how a caller correlates without
// holding library state.
struct FrameSubmit {
    uint64_t frameId = 0;
    uint64_t pts     = 0;

    // The samples.
    ExternalImage image;

    bool    forceIdr    = false;
    bool    isLastFrame = false;
    bool    hasQpOverride = false;
    int32_t qpOverride  = 0;

    ArrayView<SemaphoreWait> waitSemaphores;
    ArrayView<SemaphoreWait> signalSemaphores;

};

// An encoded frame. The bitstream points into library storage and stays valid
// until Release(frameId); a caller that needs it longer copies it.
struct EncodedFrame {
    uint64_t frameId = 0;
    uint64_t pts = 0;
    uint64_t dts = 0;

    ArrayView<uint8_t> bitstream;

    PictureType pictureType    = PictureType::Intra;
    bool        isIdr          = false;
    uint32_t    temporalLayerId = 0;

    // WHAT HAPPENED TO THIS FRAME. Ok means |bitstream| is its encode.
    //
    // Timeout means the frame was dropped against its deadline: it is still
    // delivered, still must be Released, and carries no bitstream. A caller
    // that treats every delivered frame as an encode emits an empty one and
    // calls it output.
    //
    // This is a property of the frame, not of the Acquire call, which is why
    // it rides here rather than in the Result of the acquisition: acquiring a
    // dropped frame succeeded.
    ResultCode outcome = ResultCode::Ok;
};

struct InputFormat {
    VkFormat   format     = VK_FORMAT_UNDEFINED;
    ColorModel colorModel = ColorModel::FromFormat;

    // What accepting this format costs. A format that resolves to
    // InputPath::ComputeFilter is supported but not free.
    InputPath path      = InputPath::Direct;
    bool      isOptimal = true;
};

struct QpRange {
    int32_t minQp = 0;
    int32_t maxQp = 0;
    bool    known = false;
};

struct RateControlCaps {
    bool     supportsCbr = false;
    bool     supportsVbr = false;
    bool     supportsConstantQp = false;
    uint32_t maxQualityLevels = 0;

    // The highest bitrate the device accepts for this profile, in bits/sec.
    // Zero means the device states no ceiling. A host that clamps its own
    // request needs this before it builds a configuration, which is why it is
    // a capability rather than a refusal at session creation.
    uint32_t maxBitrate = 0;
};

//=============================================================================
// PLATFORM-SCOPE INTERFACES
//=============================================================================

// What the device can do, answerable before any session exists. Every view
// returned here is owned by the platform and valid for its lifetime.
class IEncoderCaps : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IEncoderCaps/1";

    // Empty when no device reachable from this platform can encode. A
    // platform is created from an instance, which succeeds before any device
    // is examined, so this is the test for "there is an encoder here at all".
    virtual ArrayView<Codec>       Codecs() const = 0;
    virtual ArrayView<Profile>     Profiles(Codec codec) const = 0;

    // The input formats this profile accepts, each tagged with the path it
    // resolves to. A caller picks a format whose path it is willing to pay for
    // instead of discovering the cost at session creation.
    virtual ArrayView<InputFormat> InputFormats(Profile profile) const = 0;
    virtual ArrayView<uint64_t>    DrmModifiers(Profile profile, VkFormat format) const = 0;

    virtual RateControlCaps RateControl(Profile profile) const = 0;

    virtual VkExtent2D MinCodedExtent(Profile profile) const = 0;
    virtual VkExtent2D MaxCodedExtent(Profile profile) const = 0;

    virtual bool Supports(Feature feature) const = 0;
};

// The Vulkan objects the encoder runs on, whether the caller supplied them or
// the library created them.
class IDeviceBinding : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IDeviceBinding/1";

    virtual VkInstance       Instance() const = 0;
    virtual VkPhysicalDevice PhysicalDevice() const = 0;
    virtual VkDevice         Device() const = 0;
    virtual uint32_t         EncodeQueueFamilyIndex() const = 0;
    virtual uint32_t         ComputeQueueFamilyIndex() const = 0;
    virtual PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const = 0;

    // WHICH device this is, by the identity the Vulkan driver reports rather
    // than by the handle. A host that adopted a device needs to confirm the
    // encoder bound the one it meant: on a multi-GPU machine the wrong answer
    // is not an error here, it is every subsequent import failing for a reason
    // that names a buffer instead of a device.
    //
    // Handles cannot answer this. A VkPhysicalDevice from one instance and one
    // from another are different values for the same hardware, and the encoder
    // may hold its own instance.
    //
    // False when the device cannot be identified, in which case |outUuid| is
    // untouched.
    virtual bool DeviceUuid(uint8_t outUuid[VK_UUID_SIZE]) const = 0;
};

// A configuration under construction. The library owns it, so it can answer
// questions about itself rather than being an inert record the caller fills
// and hopes about.
class IEncoderConfig : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IEncoderConfig/1";

    virtual IEncoderConfig& SetCodedExtent(uint32_t width, uint32_t height) = 0;
    virtual IEncoderConfig& SetInputExtent(uint32_t width, uint32_t height) = 0;
    virtual IEncoderConfig& SetFrameRate(uint32_t numerator, uint32_t denominator) = 0;
    virtual IEncoderConfig& SetInputFormat(VkFormat format, ColorModel colorModel) = 0;
    virtual IEncoderConfig& SetRateControl(const RateControl& rateControl) = 0;
    virtual IEncoderConfig& SetGop(const GopStructure& gop) = 0;
    virtual IEncoderConfig& SetColourInfo(const ColourInfo& colour) = 0;

    // HDR10 static metadata, written into the parameter sets of an H.265 or
    // AV1 stream. H.264 defines no mastering-display or content-light SEI, so
    // Validate refuses an H.264 configuration that sets it rather than
    // accepting metadata the stream cannot carry.
    virtual IEncoderConfig& SetHdrMetadata(const HdrMetadata& metadata) = 0;

    // Refuse a session whose device lacks the extensions the import paths
    // need, naming the missing one.
    //
    // Off, a device missing them is accepted here and fails at the first
    // import, where the message is about a handle rather than about the
    // device -- so a host that intends to import anything asks for this and
    // learns at session creation instead.
    virtual IEncoderConfig& RequireImportExtensions(bool require) = 0;

    // Write the bitstream to this path. Absent, the encoder keeps every frame
    // in memory for IBitstreamSource, which is what an embedding host wants:
    // it has nowhere to write and no reason to.
    virtual IEncoderConfig& SetOutputPath(const char* path) = 0;

    virtual Codec   GetCodec() const = 0;
    virtual Profile GetProfile() const = 0;

    // Which path this configuration resolves to, and the format the session
    // will actually run at. Both are decided here so the rule has a single
    // implementation and a caller can log the answer instead of predicting it.
    virtual InputPath ResolveInputPath() const = 0;
    virtual VkFormat  ResolveSessionFormat() const = 0;

    // Everything checkable without a session, checked in one place.
    virtual Result Validate() const = 0;
};

//=============================================================================
// SESSION-SCOPE INTERFACES
//=============================================================================

// Hand frames to the encoder.
class IFrameSubmitter : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IFrameSubmitter/1";

    // Submit one frame. Returns once the work is queued, not once it is
    // encoded; the frame is complete when it reaches FrameState::Ready.
    //
    // An fd-backed image (ExternalImage) is imported here, and its fd
    // follows ExternalImage::ownership whatever this returns -- including
    // NotReady, which under Transfer has consumed the fd: a caller that
    // retries exports a new one.
    virtual Result SubmitFrame(const FrameSubmit& frame) = 0;

    // Drop a submitted frame that has not been encoded yet. A frame already
    // Ready is not cancellable and must be acquired and released.
    virtual Result CancelFrame(uint64_t frameId) = 0;

    virtual FrameState GetFrameState(uint64_t frameId) const = 0;
};

// Take encoded frames out.
//
// Every successful Acquire is paired with a Release. Until Release, the
// bitstream storage for that frame is pinned, and an encoder that runs out of
// storage stops accepting submissions.
class IBitstreamSource : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IBitstreamSource/1";

    // The next frame in encode order, or ResultCode::NotReady when none is ready.
    virtual Expected<EncodedFrame> AcquireNext() = 0;

    // A specific frame by the id its submission carried. ResultCode::NotReady
    // while it is still encoding; ResultCode::InvalidArgument if it was never
    // submitted or has already been released.
    virtual Expected<EncodedFrame> Acquire(uint64_t frameId) = 0;

    virtual void Release(uint64_t frameId) = 0;
};

// An encoding session: a configured encoder with frames in flight.
//
// The session is lifecycle only. Everything a caller does with frames is on a
// role reached through Query, so a caller depends on the four methods it uses
// rather than on every method the encoder has.
class IEncoderSession : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IEncoderSession/1";

    // Apply what can change mid-stream and refuse what cannot, naming it.
    //
    // Rate control and frame rate move, and take effect at the next encoded
    // frame rather than at an IDR boundary. Everything else -- resolution,
    // profile, input format, colour, GOP structure, quality level -- is
    // settled in the sequence header written once, or in the input routing
    // the session was built around, so changing one needs a new session. Such
    // a change is refused with ResultCode::UnsupportedFeature, the session
    // keeps running exactly as it was, and the Result's detail names the
    // offending field.
    //
    // A FIELD LEFT UNSET MEANS "LEAVE IT ALONE", not "set it to zero". The
    // session holds the configuration it was created with and overlays only
    // what this one states, so a caller may pass either the configuration it
    // built the session from with the rate changed, or a fresh one carrying
    // nothing but the new rate. Without that rule, moving the frame rate
    // would mean restating the resolution, the format and the bitrate purely
    // to avoid being refused for changing them.
    //
    // The rate control MODE is not among what moves: a session created
    // without rate control cannot be moved into CBR, only re-created.
    virtual Result Reconfigure(const Ref<IEncoderConfig>& config) = 0;

    // Encode everything submitted and wait for it, WITHOUT ending the
    // stream. The session stays fully usable: further submissions are
    // accepted and behave exactly as they would have without the drain, and
    // a caller may drain as often as it likes.
    //
    // This is the one to reach for. It is what "encode what I have given you
    // so far" means.
    virtual Result Drain() = 0;

    // END the stream: encode everything pending, write the trailing bitstream
    // a reordered GOP still owes, and release the encoder.
    //
    // TERMINAL, and named so rather than "flush" because that name invites a
    // caller to treat it as a checkpoint. After this, a submission is refused
    // with NotConfigured.
    // Frames already encoded stay acquirable and their bitstream pointers
    // stay valid, so a caller finishes collecting after finishing the stream.
    virtual Result Finish() = 0;

    // Discard everything in flight. Returns how many frames were dropped.
    // Frames already Ready are not dropped.
    virtual Expected<uint32_t> AbandonAll() = 0;
};

//=============================================================================
// PLATFORM
//=============================================================================

// The root object: a device the encoder can run on. Everything else is
// created from here.
class IEncoderPlatform : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IEncoderPlatform/1";

    virtual Ref<IEncoderCaps>   Caps() const = 0;
    virtual Ref<IDeviceBinding> DeviceBinding() const = 0;

    virtual Expected<Ref<IEncoderConfig>>  CreateConfig(Codec codec, Profile profile) = 0;
    virtual Expected<Ref<IEncoderSession>> CreateSession(const Ref<IEncoderConfig>& config) = 0;
};

} // namespace enc
} // namespace video
} // namespace vk

//=============================================================================
// THE EXPORTED SYMBOLS
//
// Two, and together they are the whole ABI: VkEncCreatePlatform below, and
// VkEncClassifyFormat after it.
//
// A host that treats the encoder as an optional dependency -- delay-loaded on
// Windows, dlopen'd elsewhere -- resolves them by name. The absence of
// VkEncCreatePlatform is how such a host decides to run without an encoder,
// so both are extern "C" and their signatures are fixed.
//=============================================================================

extern "C" VK_ENC_EXPORT
VkResult VkEncCreatePlatform(const vk::video::enc::PlatformCreateInfo& createInfo,
                             vk::video::enc::Ref<vk::video::enc::IEncoderPlatform>& outPlatform);

// How a format reaches the encoder, answered without a platform.
//
// The second of the two, and it is separate from everything above for a
// reason a caller feels: this is a property of the format, so
// requiring a platform to ask would mean creating a device to answer a
// question that does not depend on one. A host picks its buffer format while
// negotiating with a producer, long before the encoder exists.
//
// Returns VK_ERROR_FORMAT_NOT_SUPPORTED, with outRouting->supported false, for
// a format this library cannot take on any route -- including one that only
// the compute filter could convert, which IEncoderConfig::Validate refuses
// for the same reason: a submitted frame is read directly or copied.
extern "C" VK_ENC_EXPORT
VkResult VkEncClassifyFormat(VkFormat                     format,
                             vk::video::enc::ColorModel   colorModel,
                             vk::video::enc::FormatRouting* outRouting);

#endif /* _VULKAN_VIDEO_ENCODER_H_ */
