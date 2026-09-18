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
// detail() is a literal, never a formatted message. Anything needing runtime
// context -- which frame, which format -- belongs on IDiagnostics.
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

// How the library obtains an input image the caller already owns.
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
    DmaBufImport = 0,
    ExternalSemaphores,
    IntraRefresh,
    HdrMetadata,
    Reconfigure,
    RegisteredResources,
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
    uint32_t gopLength          = 0;   // frames; 0 means infinite
    uint32_t idrPeriod          = 0;   // frames; 0 means IDR only at the start
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

// A registered image or semaphore, named by an opaque value the library
// chooses. Zero is never a live registration, so a caller may use it as
// "none" without a companion flag.
//
// Opaque rather than an index into anything: a caller that could compute one
// could also collide with one, and the library is free to change what it
// stores behind it.
typedef uint64_t ResourceId;
const ResourceId kNoResource = 0;

// What happens to an OS handle the caller passes in.
//
// This is not a detail. Under Transfer the library closes the handle on
// EVERY exit path, including a failed registration, so a caller that also
// closes it double-closes -- and a double close is not a leak, it is a
// close of whatever unrelated descriptor has since taken the number.
enum class HandleOwnership : uint32_t {
    // The library takes the handle. The caller must not close it, on any
    // outcome, including a failed registration.
    //
    // Who closes it in the end depends on how far the import got, and a
    // caller can neither observe nor rely on which: once the handle has been
    // handed to the driver, the driver owns it -- even if the allocation then
    // fails -- and the library will not close it, because that would be a
    // second close of a number this process may already have recycled. Short
    // of that handoff the library closes it. Either way it is not yours.
    //
    // The default, because it is what the layer below does and what a
    // producer handing over a dma-buf almost always wants: one owner.
    Transfer = 0,
    // The caller keeps the handle and closes it once registration returns.
    // The library duplicates whatever it needs.
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

// An image the caller already owns, described well enough for the library to
// import it without guessing. Only the fields the handle type needs are read.
struct ExternalImage {
    ExternalHandleType handleType = ExternalHandleType::NoHandle;

    // The OS handle, or the Vulkan image when handleType is VkImageHandle.
    //
    // WHO OWNS fd IS |ownership|, AND IT DEFAULTS TO Transfer: once passed,
    // it is not yours to close on any outcome. Set Borrow to keep it -- the
    // library then works from a private duplicate taken before anything that
    // can fail. A Win32 handle is never closed by the library under either
    // rule, and a VkImage is not a handle this rule applies to.
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

    // The usage and flags the image was created with. The library cannot
    // discover these from a VkImage handle, and it needs them to decide
    // whether the encoder can read the image directly or must stage it, so
    // leaving them zero makes an otherwise valid registration fail.
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

    // WHICH DEVICE ALLOCATED THIS, so the library can refuse an image that
    // belongs to another one. On a multi-GPU host an import that silently
    // succeeds against the wrong device is worse than a refusal: it fails
    // later, in the driver, with nothing naming the cause.
    //
    // ALL-ZERO MEANS "NOT STATED", and is what a caller that genuinely does
    // not know passes. The library then skips the check, so leaving these
    // empty is not the safe default -- it is the unchecked one.
    uint8_t deviceUUID[VK_UUID_SIZE] = {};
    uint8_t driverUUID[VK_UUID_SIZE] = {};
    uint8_t deviceLUID[VK_LUID_SIZE] = {};
    bool    deviceLuidValid = false;
};

// A semaphore the caller owns elsewhere, imported by OS handle. Timeline
// semaphores only: a binary semaphore cannot express "frame N is done" to a
// consumer that was not waiting at the moment it was signalled.
//
// Who closes the handle is |ownership| below, and it defaults to Transfer.
struct ExternalSemaphore {
    ExternalHandleType handleType  = ExternalHandleType::NoHandle;
    int                fd          = -1;
    void*              win32Handle = nullptr;

    // As for an image: Transfer means the library closes fd, including on
    // failure.
    HandleOwnership ownership = HandleOwnership::Transfer;
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

    // Exactly one of these describes the samples: an image registered earlier
    // with IResourceRegistry, or an image described inline. A registered image
    // costs no import here, which is why a compositor recycling buffers
    // registers them once.
    ResourceId    registeredImage = kNoResource;
    ExternalImage image;

    bool    forceIdr    = false;
    bool    isLastFrame = false;
    bool    hasQpOverride = false;
    int32_t qpOverride  = 0;

    ArrayView<SemaphoreWait> waitSemaphores;
    ArrayView<SemaphoreWait> signalSemaphores;

    // ---- fences, for a producer this encoder does not share a queue with ----

    // Wait for this fence before reading the image. A compositor handing over
    // a buffer it has just rendered into passes the fence that render
    // signalled; without it the encode may read the frame half-written.
    //
    // THE LIBRARY CLOSES THIS FD, on every exit path including a refusal, and
    // does NOT write -1 back. A caller that re-submits the same frame must
    // therefore pass a fresh descriptor each time -- in practice a dup, so
    // that the caller keeps the original and every attempt waits properly.
    // -1 means no ordering is required.
    int acquireFenceFd = -1;

    // Receives a fence signalled when the encoder has finished reading the
    // image, so the caller knows when it may write the buffer again.
    //
    // The library writes -1 through this pointer before anything in the
    // submission can refuse, so the storage never keeps a stale fd from a
    // previous attempt. The caller owns whatever it receives and closes it.
    // Null asks for no release fence.
    int* releaseFenceFd = nullptr;
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

// What the session has done and what it is holding. A caller watching for a
// stall reads this: a completion counter that does not move while frames are
// outstanding and output buffers are free is the signature of one.
//
// The descriptor API answers this as two chained structures; here it is one
// value, because a caller that wants the counters always wants the
// diagnostics alongside them.
struct CompletionStats {
    uint64_t completionCounter = 0;   // monotonic; the drain target
    uint64_t framesTimedOut    = 0;   // deadline drops delivered
    uint64_t lateCaptures      = 0;   // captures discarded after a timeout drop
    uint64_t framesCancelled   = 0;

    uint32_t framesPending  = 0;      // submitted, not yet encoded
    uint32_t framesReady    = 0;      // retrievable now
    uint32_t framesAcquired = 0;      // delivered, awaiting Release

    // Misuses the library has recorded, and the most recent one. A non-zero
    // count is a caller bug the library chose to survive rather than refuse.
    uint64_t diagnosticCount = 0;
    char     lastDiagnostic[256] = {};
};

struct RuntimeInfo {
    char implementationName[64] = {};

    bool isHardwareAccelerated = false;
    // The encoder can take a frame by handle rather than by copy. A host
    // decides whether to allocate importable buffers on this.
    bool supportsNativeHandle = false;
    // The rate controller is the hardware's, so a host must not second-guess
    // it by re-driving the bitrate per frame.
    bool trustedRateController = false;
    bool supportsSimulcast = false;
    // Resolution can change without rebuilding the session.
    bool supportsFrameSizeChange = false;
    bool reportsAverageQp = false;

    // What the encoder wants the coded extent rounded to. One means no
    // constraint; a host that ignores these gets a refusal at session
    // creation rather than a silent crop.
    uint32_t resolutionAlignmentWidth  = 1;
    uint32_t resolutionAlignmentHeight = 1;
    // Whether that alignment applies to every simulcast layer or only the
    // base one.
    bool applyAlignmentToAllSimulcastLayers = false;
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

// HDR10 static metadata, reachable through Query on a configuration whose
// codec can carry it. H.264 has no mastering-display or content-light SEI, so
// Query returns null for an H.264 configuration and the caller learns that
// without creating a session.
class IHdrMetadataConfig : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IHdrMetadataConfig/1";

    virtual void SetHdrMetadata(const HdrMetadata& metadata) = 0;
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

// Learn that frames have completed without polling.
//
// A caller uses exactly one of these styles. The callback runs on a library
// thread and must not call back into the session; it exists to wake the
// caller's own loop.
class ICompletionSignal : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.ICompletionSignal/1";

    typedef void (*CompletionCallback)(uint64_t frameId, void* userData);

    // Invoked exactly once when the encoder is done with a |userData| it was
    // given -- on replacement, on detach, or at teardown -- and only after any
    // invocation still running has returned.
    typedef void (*UserDataRelease)(void* userData);

    // Install, replace, or (with a null callback) clear the completion
    // callback.
    //
    // |release|, when given, hands ownership of |userData| to the encoder.
    // Prefer it to keeping the cookie alive by arrangement with teardown
    // order: the encoder knows when the last invocation has returned and the
    // caller does not.
    //
    // Clearing is a quiesce point -- once it returns, no invocation is in
    // flight -- so a caller that manages the cookie itself may destroy it
    // immediately afterwards.
    virtual Result SetCallback(CompletionCallback callback, void* userData,
                               UserDataRelease release = nullptr) = 0;

    // Monotonic count of frames that have become Ready. A caller that samples
    // it needs no callback and no handle.
    virtual uint64_t CompletedCount() const = 0;

    // A semaphore the library signals as frames complete, for a caller that
    // already waits on Vulkan. Null when the implementation has none.
    virtual VkSemaphore CompletionSemaphore() const = 0;

    // An OS handle to wait on from a non-Vulkan loop.
    //
    // Each call returns a NEW handle and the caller closes that one. The
    // library keeps its own and closes it at teardown, so closing an exported
    // handle never disturbs the encoder, and the encoder's teardown never
    // invalidates a handle a caller still holds.
    //
    // Exports share the underlying completion object. They are independent
    // close obligations, not independent queues: a completion drained through
    // one export is not redelivered to another. Waking on any of them means
    // "drain what is available and reconcile against CompletedCount()", which
    // is the same discipline a single handle needs, because the platforms
    // differ in what an unread signal accumulates to.
    //
    // The handle is close-on-exec where the platform expresses that.
    virtual Expected<int> ExportCompletionHandle() = 0;
};

// What a terminal Finish() actually proved.
//
// Finish() returning an error and the GPU still holding the caller's input are
// different questions, and a single Result cannot answer both. A producer that
// lent the encoder an image asks this one before it takes the image back.
enum class ShutdownDisposition : int32_t {
    // Every library worker joined, the whole-device wait succeeded and no
    // device loss was seen. The input may be released -- even when Finish()
    // reported an earlier encode or file-output failure. Bitstreams already
    // copied out stay valid until Release().
    Idle = 0,

    // The workers joined and the device was lost. Pending Vulkan use is
    // retired, so nothing is still reading -- but shared external contents and
    // peer image layouts are not valid, which is not the same permission.
    LostDeviceRetired,

    // Any other terminal wait failure, or a shutdown still underway. Nothing
    // established that the device stopped using the images, imported
    // semaphores and registrations involved: keep every one of them, and every
    // producer-side dependency, alive. Cancelling or abandoning frames does
    // not change this.
    Unproven,
};

// Thread-safe to read at any time; meaningful after Finish() returns.
struct ShutdownSnapshot {
    ShutdownDisposition disposition = ShutdownDisposition::Unproven;

    // The FIRST encode or shutdown failure, kept. A clean later step does not
    // clear it.
    ResultCode firstError = ResultCode::Ok;

    bool workersJoined    = false;
    bool callbackDetached = false;

    // Sticky, and separate from the disposition on purpose: a lost device
    // followed by a wait that returns success is still a lost device.
    bool deviceLostObserved = false;

    // False while a shutdown is still underway or was left Unproven.
    bool complete = false;
};

// Why Finish()'s Result is not enough, and what to do about it.
class IShutdownDiagnostics : public IObject {
public:
    static constexpr std::string_view kId =
        "vk.video.enc.IShutdownDiagnostics/1";

    virtual ShutdownSnapshot Shutdown() const = 0;
};

// Register images and semaphores once and refer to them by index afterwards.
//
// This exists because importing a dma-buf costs a Vulkan image creation, and a
// compositor recycles the same handful of buffers for the life of a stream.
// Registration moves that cost out of the per-frame path.
class IResourceRegistry : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IResourceRegistry/1";

    // Returns the id to put in FrameSubmit::registeredImage.
    //
    // |outHandleConsumed|, when given, is WRITTEN ON EVERY RETURN, including
    // every refusal -- which is the only reason it is an out-parameter rather
    // than part of the returned value. Under HandleOwnership::Transfer a
    // refusal still consumes the handle, so a caller that learns this only on
    // success learns it exactly when it does not matter.
    //
    // DIAGNOSTIC ONLY. A false under Transfer is a library defect, and still
    // not an instruction to close: the caller cannot know how far the import
    // got, so closing on it risks the double close the ownership rule exists
    // to prevent. Assert on it; do not act on it.
    virtual Expected<ResourceId> RegisterImage(
        const ExternalImage& image,
        bool*                outHandleConsumed = nullptr) = 0;
    virtual Result               UnregisterImage(ResourceId id) = 0;

    // Whether this image could be registered, and what it would cost, without
    // registering it. A caller uses this to choose a buffer format before it
    // allocates.
    virtual Expected<InputPath> QueryImageSupport(const ExternalImage& image) = 0;

    virtual Expected<ResourceId> RegisterSemaphore(const ExternalSemaphore& semaphore) = 0;
    virtual Result               UnregisterSemaphore(ResourceId id) = 0;
};

// What the implementation is and what it did. Nothing here changes encoder
// behaviour; it is for logs and for the runtime-context detail that Result
// deliberately does not carry.
class IDiagnostics : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IDiagnostics/1";

    virtual RuntimeInfo GetRuntimeInfo() const = 0;

    // The last error in full, with the context Result omits. Valid until the
    // next call that returns a failing Result.
    virtual const char* LastErrorDetail() const = 0;

    virtual uint64_t FramesSubmitted() const = 0;
    virtual uint64_t FramesEncoded() const = 0;

    // The session's own accounting, which is authoritative where the two
    // counters above are only this interface's view of it.
    virtual Expected<CompletionStats> GetCompletionStats() const = 0;
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
    // keeps running exactly as it was, and IDiagnostics::LastErrorDetail
    // names the offending field.
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
    // with NotConfigured and CompletionSemaphore() answers VK_NULL_HANDLE.
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

// Releasing the process-wide Vulkan instance the encoder stands up.
//
// An OWN-mode platform caches its VkInstance for the process lifetime so that
// repeated create/destroy cycles reuse one instance rather than issuing a
// second vkCreateInstance -- which a sandboxed process may no longer be
// permitted to do. Dropping every Ref therefore does NOT destroy the instance,
// and a process that exits without calling Retire() leaves it standing. Under
// a driver that audits allocations at exit, that is reported as a leak.
//
// Retire() is how a caller that owns the process lifetime releases it at a
// moment of its choosing: late enough that no encoding remains, early enough
// that calling the driver is still allowed. It is permanent -- after it, every
// CreateSession and every VkEncCreatePlatform for this device fails rather
// than standing a second instance up -- and idempotent.
//
// A caller that simply runs to process exit need not call it at all.
class IPlatformLifetime : public IObject {
public:
    static constexpr std::string_view kId = "vk.video.enc.IPlatformLifetime/1";

    // Returns the number of cached device contexts released; zero if the
    // instance was already retired.
    virtual uint32_t Retire() = 0;
};

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
// a format this library cannot take on any route.
extern "C" VK_ENC_EXPORT
VkResult VkEncClassifyFormat(VkFormat                     format,
                             vk::video::enc::ColorModel   colorModel,
                             vk::video::enc::FormatRouting* outRouting);

#endif /* _VULKAN_VIDEO_ENCODER_H_ */
