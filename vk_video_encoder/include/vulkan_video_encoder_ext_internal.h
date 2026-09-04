/*
 * Internal to the encoder library and its tests. NOT part of the public ABI:
 * nothing here may be relied on by a consumer, and it carries no sType.
 *
 * Two layers of machinery, described below, that make
 * "accepted and ignored" structurally detectable rather than a thing anyone
 * has to remember.
 *
 * Layer 1 is the field table below. Every field of VkVideoEncoderConfig
 * appears exactly once with a disposition saying what happens to it. Each
 * entry carries an offsetof assertion, so renaming or removing a field fails
 * the build here; and the sizeof assertion on the struct fails the build when
 * a field is ADDED, forcing the author to this table to classify it. The two
 * together mean a new field cannot reach a release without someone deciding,
 * in writing, what it does.
 *
 * Layer 3 is the binder conformance suite, which drives
 * VkEncBuildAndProbeConfig per codec arm and asserts effect or explicit
 * rejection for every BOUND field except outputPath, which has no probe
 * projection: its binding is a conditional file-open, and a consumer that
 * captures in memory nulls the field. Those assertions are written by hand;
 * no test walks this table pairing dispositions with effects, so a new
 * BOUND field is covered only once its author adds the assertion. The one
 * test that does iterate the table (FieldTableIsExhaustiveAndClassified)
 * checks its shape -- exactly-once classification, in-struct offsets -- not
 * field effect. The binder is exposed as a free function because it touches
 * no member state, so the suite drives it with no Vulkan device at all.
 */

#ifndef VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_
#define VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_

#include <cstddef>
#include <cstdint>

// Sibling header, same directory. A quoted include resolves against the
// including file's own directory before any -I, so this needs no shared -I set
// between the library target and a consumer's test target -- and, unlike a
// src-root-relative path, it assumes no particular checkout layout.
// Deliberately does NOT reach into the library's private headers -- see
// VkEncBoundConfigProbe.
#include "vulkan_video_encoder_ext.h"

// Disposition of a public config field.
//
//   BOUND    -- forwarded into EncoderConfig by the binder. The binder suite
//               asserts the landing for every BOUND field except outputPath
//               (no probe projection; see the file comment above).
//   SESSION  -- consumed by the ext layer itself (device/instance selection,
//               stdio silencing, codec dispatch). Never reaches EncoderConfig,
//               and must not: it shapes the session, not the encode.
//   ABI_GATE -- consumed by the versioning gate before anything else runs.
//   REJECTED -- a non-default value is refused at init, because honouring it
//               is impossible in this build. Silently ignoring it is the
//               defect; the rejection is the fix.
enum VkVideoEncoderConfigFieldDisposition {
    VK_ENC_FIELD_BOUND = 0,
    VK_ENC_FIELD_SESSION,
    VK_ENC_FIELD_ABI_GATE,
    VK_ENC_FIELD_REJECTED,
};

// X(field, disposition, note)
#define VK_VIDEO_ENCODER_CONFIG_FIELDS(X)                                      \
    X(sType,                    ABI_GATE, "structure-type gate")               \
    X(pNext,                    ABI_GATE, "extension chain, walked at init")   \
    X(codec,                    SESSION,  "selects the codec config subclass") \
    X(profile,                  BOUND,    "the codec standard's own profile " \
                                          "number; a number this library "    \
                                          "cannot bind, or one the input "    \
                                          "depth does not admit, is refused") \
    X(encodeWidth,              BOUND,    "cfg->encodeWidth")                  \
    X(encodeHeight,             BOUND,    "cfg->encodeHeight")                 \
    X(inputFormat,              BOUND,    "validated, then cfg->input.bpp")    \
    X(inputWidth,               BOUND,    "cfg->input.width")                  \
    X(inputHeight,              BOUND,    "cfg->input.height")                 \
    X(rateControlMode,          BOUND,    "cfg->rateControlMode")              \
    X(averageBitrate,           BOUND,    "cfg->averageBitrate")               \
    X(maxBitrate,               BOUND,    "cfg->maxBitrate")                   \
    X(vbvBufferSize,            BOUND,    "cfg->vbvBufferSize")                \
    X(constQpI,                 BOUND,    "cfg->constQp.qpIntra "              \
                                          "+ constQpSet")                      \
    X(constQpP,                 BOUND,    "cfg->constQp.qpInterP "             \
                                          "+ constQpSet")                      \
    X(constQpB,                 BOUND,    "cfg->constQp.qpInterB "             \
                                          "+ constQpSet")                      \
    X(minQp,                    BOUND,    "cfg->minQp + minQpSet (H.26x); "  \
                                          "rejected on AV1 (qIndex units)")   \
    X(maxQp,                    BOUND,    "cfg->maxQp + maxQpSet (H.26x); "  \
                                          "rejected on AV1 (qIndex units)")   \
    X(gopLength,                BOUND,    "gopStructure.SetGopFrameCount")     \
    X(consecutiveBFrames,       BOUND,    "SetConsecutiveBFrameCount")         \
    X(idrPeriod,                BOUND,    "gopStructure.SetIdrPeriod")         \
    X(closedGop,                BOUND,    "gopStructure.SetClosedGop")         \
    X(frameRateNum,             BOUND,    "cfg->frameRateNumerator")           \
    X(frameRateDen,             BOUND,    "cfg->frameRateDenominator")         \
    X(qualityLevel,             BOUND,    "cfg->qualityLevel")                 \
    X(tuningMode,               BOUND,    "cfg->tuningMode, validated")        \
    X(colourPrimaries,          BOUND,    "cfg->colour_primaries (VUI)")       \
    X(transferCharacteristics,  BOUND,    "cfg->transfer_characteristics")     \
    X(matrixCoefficients,       BOUND,    "cfg->matrix_coefficients")          \
    X(videoFullRange,           BOUND,    "cfg->video_full_range_flag")        \
    X(enablePreprocessFilter,   BOUND,    "cfg->enablePreprocessComputeFilter"\
                                          "; WAS REJECTED. The refusal was "  \
                                          "right while nothing converted: "   \
                                          "accepting it then would have "     \
                                          "encoded unconverted input as if "  \
                                          "converted. The conversion is now " \
                                          "real for YCbCr plane-count "       \
                                          "mismatches -- per-plane storage "  \
                                          "views on the import, per-frame "   \
                                          "routing instead of the "           \
                                          "isExternalInput bypass, the "      \
                                          "filter branch's acquire barriers, "\
                                          "and one queue fact shared by the " \
                                          "barriers and the submit -- so the "\
                                          "field is FORWARDED. Refused, with "\
                                          "a reason, where it cannot be "     \
                                          "honoured: no filter compiled in, " \
                                          "or no compute queue on the "       \
                                          "session's device. RGBA input "     \
                                          "remains unsupported: it needs the "\
                                          "sampled-read arm this does not "   \
                                          "deliver. filterType is NOT bound " \
                                          "from any public field -- the "     \
                                          "library derives it from the input "\
                                          "and encode-source formats")        \
    X(deviceId,                 SESSION,  "physical-device selection")         \
    X(gpuUUID,                  SESSION,  "physical-device selection")         \
    X(outputPath,              BOUND,    "cfg->outputFileHandler")             \
    X(verbose,                  BOUND,    "cfg->verbose")                      \
    X(validate,                 BOUND,    "cfg->validate")                     \
    X(disableFileOutput,        BOUND,    "cfg->disableFileOutput")            \
    X(silenceStdio,             SESSION,  "SetVkEncoderStdioSilenced")         \
    X(externalInstance,         SESSION,  "caller-supplied instance")          \
    X(externalPhysicalDevice,   SESSION,  "caller-supplied physical device")   \
    X(externalDevice,           SESSION,  "caller-supplied logical device")    \
    X(externalEncodeQueueFamilyIndex,  SESSION,                                \
                                "honoured only with externalDevice")           \
    X(externalComputeQueueFamilyIndex, SESSION,                                \
                                "honoured only with externalDevice")

// Stable index per field. No test iterates by these indices today; the one
// live consumer is kVkEncCfgFieldCount, which pins the table length (the
// static_assert below, re-checked by FieldTableIsExhaustiveAndClassified).
#define VK_ENC_FIELD_ENUM(field, disposition, note) kVkEncCfgField_##field,
enum VkVideoEncoderConfigFieldId {
    VK_VIDEO_ENCODER_CONFIG_FIELDS(VK_ENC_FIELD_ENUM)
    kVkEncCfgFieldCount
};
#undef VK_ENC_FIELD_ENUM

struct VkVideoEncoderConfigFieldInfo {
    const char*                             name;
    VkVideoEncoderConfigFieldDisposition    disposition;
    const char*                             note;
    size_t                                  offset;
};

// Every field's existence is asserted by taking its offset: a rename or a
// removal stops compiling here, which is the point.
#define VK_ENC_FIELD_INFO(field, disposition, note)                            \
    {#field, VK_ENC_FIELD_##disposition, note,                                 \
     offsetof(VkVideoEncoderConfig, field)},
static const VkVideoEncoderConfigFieldInfo kVkVideoEncoderConfigFields[] = {
    VK_VIDEO_ENCODER_CONFIG_FIELDS(VK_ENC_FIELD_INFO)
};
#undef VK_ENC_FIELD_INFO

static_assert(sizeof(kVkVideoEncoderConfigFields) /
                      sizeof(kVkVideoEncoderConfigFields[0]) ==
                  (size_t)kVkEncCfgFieldCount,
              "field table and field enum disagree");

// A flat projection of everything the binder is supposed to have written.
// The binder suite asserts on this rather than on EncoderConfig, so the
// tests depend on the binding CONTRACT and not on the library's internal
// layout -- and the library keeps its private headers private.
struct VkEncBoundConfigProbe {
    uint32_t encodeWidth;
    uint32_t encodeHeight;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t inputBpp;
    uint32_t rateControlMode;
    uint32_t averageBitrate;
    uint32_t maxBitrate;
    uint32_t vbvBufferSize;
    uint32_t constQpIntra;
    uint32_t constQpInterP;
    uint32_t constQpInterB;
    int32_t  minQp;
    int32_t  maxQp;
    uint32_t minQpSet;
    uint32_t maxQpSet;
    uint32_t constQpSet;
    uint32_t gopFrameCount;
    uint32_t idrPeriod;
    uint32_t consecutiveBFrames;
    uint32_t closedGop;
    uint32_t frameRateNumerator;
    uint32_t frameRateDenominator;
    uint32_t qualityLevel;
    uint32_t tuningMode;
    uint32_t colourPrimaries;
    uint32_t transferCharacteristics;
    uint32_t matrixCoefficients;
    uint32_t videoFullRangeFlag;
    uint32_t colorDescriptionPresent;
    uint32_t videoSignalTypePresent;
    uint32_t verbose;
    uint32_t validate;
    uint32_t disableFileOutput;
    // EncoderConfig::enablePreprocessComputeFilter, not a public config
    // field. This is where enablePreprocessFilter LANDS, and the projection
    // is what makes its disposition assertable in both directions: VK_FALSE
    // must write 0 here rather than inheriting EncoderConfig's default of
    // true (nothing else in the probe would notice a filter object created
    // behind the caller's back), and VK_TRUE must now write 1 rather than
    // being refused at init.
    //
    // CONTRACT CHANGE, called out because the binder conformance suite pins
    // it: this field could previously only ever read 0, because the binder
    // rejected enablePreprocessFilter == VK_TRUE outright. A suite asserting
    // "init fails when enablePreprocessFilter is set" now fails, and should:
    // the field moved from REJECTED to BOUND in the table above.
    uint32_t preprocessComputeFilter;
    // EncoderConfig::input.numPlanes. Projected because EncoderConfig does
    // not store the input format at all -- it reconstructs input.vkFormat
    // from subsampling, bit depth and this count -- so "inputFormat was
    // bound" is only assertable through the plane count it implies. Left
    // unwritten it inherits EncoderConfig's default of 3, which would
    // describe every session's input as 3-plane I420.
    uint32_t inputNumPlanes;

    // Per-codec-arm effect projections, run PER CODEC ARM. A projection
    // that stops at the shared EncoderConfig members cannot see
    // codec-conditional consumption: a field can reach the base config and
    // still have no effect on the arm that encodes. These project what each
    // arm actually hands the driver.
    // Zero when the arm was not exercised.
    //
    // H.26x: the rate-control layer info the codec arm builds. useMinQp /
    // useMaxQp are what make the clamp values legally visible to the driver.
    uint32_t rcUseMinQp;
    uint32_t rcUseMaxQp;
    int32_t  rcMinQpI;
    int32_t  rcMaxQpI;
    // AV1: the sequence-header colour config the arm attaches (AV1's
    // counterpart of the H.26x VUI colour description).
    uint32_t av1ColorDescriptionPresent;
    uint32_t av1ColorPrimaries;
    uint32_t av1TransferCharacteristics;
    uint32_t av1MatrixCoefficients;
    uint32_t av1ColorRange;
    uint32_t av1BitDepth;
    // All arms: the codec-typed config's profile, read through the same
    // virtual accessor session creation consumes (GetCodecProfile, read by
    // EncoderConfig::InitVideoProfile), so this projects the profile the
    // arm actually encodes with -- H.264 SPS profile_idc, H.265 PTL
    // general_profile_idc, AV1 seq_profile, in the codec's own Std enum
    // values. An explicit caller profile must be visible here or init must
    // have failed. NOTE: STD_VIDEO_AV1_PROFILE_MAIN == 0, so on the AV1
    // arm 0 is a real value, not "arm not exercised".
    uint32_t codecProfile;
};

// Run the binder and project the result. Reads only its arguments: no device,
// no instance, no encoder -- which is what lets the binder suite assert every
// BOUND field but outputPath from a plain gtest process.
VkResult VkEncBuildAndProbeConfig(const VkVideoEncoderConfig& extConfig,
                                  VkVideoCodecOperationFlagBitsKHR codecOp,
                                  VkEncBoundConfigProbe* outProbe);

// ---------------------------------------------------------------------------
// Input-format taxonomy.
//
// One VkBool32 used to answer two different questions, and collapsing them is
// what made CONTEXT_DESIGN 3.4.1's adaptation ladder look like a single step.
// The ladder has three rungs -- hardware conversion, compute filter, transfer
// copy -- and the FORMAT decides which rungs are even candidates:
//
//   ENCODABLE_DIRECT     the device takes this format as an encode source as
//                        it stands (semi-planar 4:2:0). Rung 1.
//   ENCODABLE_VIA_FILTER encodable only after a conversion the compute filter
//                        performs. TWO families, and they differ in more than
//                        degree:
//                          - the 3-plane 4:2:0 family, whose mismatch against
//                            the semi-planar encode format is a PLANE-COUNT
//                            mismatch; and
//                          - the 8-bit RGBA family (R8G8B8A8_UNORM,
//                            B8G8R8A8_UNORM, A8B8G8R8_UNORM_PACK32), a
//                            COLOUR-MODEL mismatch, converted by the filter's
//                            sampled-read arm. Single-plane, which is why
//                            VkEncInputFormatPlaneCount can no longer answer
//                            from the class alone.
//                        Rung 2, and rung 2 ONLY: the transfer copy cannot
//                        substitute for either. CopyLinearToOptimal-
//                        Image asserts the source has no third plane and, in a
//                        build with that assert compiled out, copies two of
//                        three planes -- which is the shape
//                        DEVICE_INDEPENDENCE_PLAN section 8.1 measured on this
//                        hardware as VK_ERROR_DEVICE_LOST, a GPU hang and a
//                        0-byte bitstream, 3 runs of 3. A copy cannot perform
//                        a colour-space conversion at all.
//   UNSUPPORTED          neither, in this build.
//
// This is a property of the FORMAT alone, so it is a free function and a test
// can drive every arm with no device. Whether a given SESSION can actually
// take an ENCODABLE_VIA_FILTER input is a second question with a second
// answer -- it needs the compute filter to be compiled in AND enabled AND
// able to read the specific image -- and it is answered by
// VulkanVideoEncoderExtImpl::SupportsFormat / ValidateImageDescriptor, which
// have the session state this function deliberately does not.
enum VkEncInputFormatClass {
    VK_ENC_INPUT_FORMAT_UNSUPPORTED = 0,
    VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
    VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
};
VkEncInputFormatClass VkEncClassifyInputFormat(VkFormat inputFormat);

// "Could this library take this format on SOME path?" -- i.e. classified as
// anything but UNSUPPORTED.
//
// Kept, and kept deliberately UNUSED inside the library, which is worth
// stating rather than leaving to be discovered: every gate needs a finer
// answer than it gives. The binder needs to know whether a format
// requires the filter, and VulkanVideoEncoderExtImpl::SupportsFormat needs
// to know whether THIS session has one. A predicate that answers "some path
// exists somewhere" is exactly the collapse this taxonomy was introduced to
// undo, so nothing in the library should reach for it again; it remains as
// the coarse question an out-of-tree consumer may still legitimately ask.
VkBool32 VkEncSupportsInputFormat(VkFormat inputFormat);

// The number of planes |inputFormat| is laid out in, which EncoderConfig
// needs in order to reconstruct input.vkFormat at all: it does not store the
// input format, it DERIVES it from chroma subsampling, bit depth and
// numPlanes (API_V2:541). 0 for a format this library does not classify.
uint32_t VkEncInputFormatPlaneCount(VkFormat inputFormat);

// Is this one of the RGBA-family inputs the compute filter converts?
//
// A strict subset of ENCODABLE_VIA_FILTER, exposed because that class now
// spans two families that disagree about plane count and about how the input
// geometry is derived. Answers VK_FALSE for every YCbCr format and for every
// format outside the taxonomy.
VkBool32 VkEncIsRgbaInputFormat(VkFormat inputFormat);

// Import memory-type selection.
//
// The candidate SET is spec-constrained; the preference ORDER within it is
// this library's policy (exporter's index if representable, else DEVICE_LOCAL,
// else the first importable type):
//   - dma-buf / D3D11: |authoritativeMask| is what
//     vkGetMemoryFd/Win32HandlePropertiesKHR reported for THIS handle
//     (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00648 / -00645). 0 means
//     the query was unavailable, which degrades to the legacy heuristic --
//     the caller logs and counts that, never this function.
//   - opaque handles (|exporterIndexExact|): the exporter's own parameters
//     are the only legal ones (VUID-VkMemoryAllocateInfo-allocationSize-
//     01742/-01743); a known-but-unrepresentable index has no valid
//     substitute and selects nothing.
//
// Free function with no device dependency, so the policy is testable in a
// device-free unit test exactly like the config binder above.
enum VkEncImportMemoryTypeOutcome {
    VK_ENC_IMPORT_MEMTYPE_NONE = 0,        // nothing importable: refuse
    VK_ENC_IMPORT_MEMTYPE_EXPORTER_INDEX,  // exporter's index, inside the mask
    VK_ENC_IMPORT_MEMTYPE_MASK_DEVICE_LOCAL,
    VK_ENC_IMPORT_MEMTYPE_MASK_FIRST,      // importable but not DEVICE_LOCAL
};

struct VkEncImportMemoryTypeRequest {
    uint32_t requirementsMask  = 0;           // vkGetImageMemoryRequirements
    uint32_t authoritativeMask = 0;           // handle-properties query; 0 = none
    uint32_t exporterMask      = 0;           // descriptor.memoryTypeBits; 0 = unknown
    uint32_t exporterIndex     = UINT32_MAX;  // descriptor.memoryTypeIndex
    VkBool32 exporterIndexExact = VK_FALSE;   // opaque handles: as-given or nothing
};

struct VkEncImportMemoryTypeChoice {
    uint32_t index = UINT32_MAX;              // valid iff outcome != NONE
    VkEncImportMemoryTypeOutcome outcome = VK_ENC_IMPORT_MEMTYPE_NONE;
    // The exporter named an index and the mask excluded it. SUCCESS-class
    // (a different type was selected) -- the caller logs and counts it.
    VkBool32 exporterIndexOverridden = VK_FALSE;
};

void VkEncSelectImportMemoryType(const VkEncImportMemoryTypeRequest& request,
                                 const VkPhysicalDeviceMemoryProperties& memProps,
                                 VkEncImportMemoryTypeChoice* outChoice);

// The acceptance behind ModifierWouldRegister's modifier pre-check: a
// modifier the device cannot service must surface as the renegotiable
// MODIFIER_UNSUPPORTED, not fall through to vkCreateImage as
// IMPORT_FAILED. vkCreateImage's validity is defined
// against the vkGetPhysicalDeviceImageFormatProperties2 query for the same
// inputs THROUGH THE LIMITS THE QUERY RETURNS: extent
// (VUID-VkImageCreateInfo-extent-02252/-02253), mip levels
// (-mipLevels-02255), array layers (-arrayLayers-02256) and sample count
// (-samples-02258), with the descriptor's 0-means-default rules applied
// exactly as ImportImageLocked applies them when it builds
// VkImageCreateInfo. Consuming fewer of them admits a descriptor the
// import must then refuse.
//
// Free function with no device dependency, so the acceptance is provable in
// a device-free unit test exactly like the memory-type policy above; only
// the query itself needs hardware.
VkBool32 VkEncDescriptorWithinCreationLimits(
    const VkVideoEncoderExternalImageDescriptor& desc,
    const VkImageFormatProperties& limits);

// ---------------------------------------------------------------------------
// The external-image import behind registration.
//
// Exposed here for the same reason as the memory-type policy above: the
// fd-ownership split below is decided against DRIVER-call failures that no
// test on working hardware can produce on demand, so the import takes the
// dispatch context by reference and a test drives its real code with a
// stubbed dispatch table and real pipe(2) fds. Only the happy path against
// a real driver needs hardware.
class VulkanDeviceContext;

// Which side of the vkAllocateMemory handoff the import stopped on. The
// split has to be carried because once vkAllocateMemory has been CALLED
// with a VkImportMemoryFdInfoKHR chained, the fd is not the library's to
// close -- NVIDIA consumes it even when the allocation FAILS -- and on
// success the VkDeviceMemory owns it until vkFreeMemory. A close() on
// either post-handoff path is a double close, and in a multithreaded
// process the number is immediately recyclable, so the second close tears
// down whatever unrelated descriptor now holds it.
enum VkEncExternalImageImportResult {
    VK_ENC_IMPORT_FAILED_BEFORE_ALLOCATE = 0,  // fd never reached the driver
    VK_ENC_IMPORT_FAILED_AFTER_ALLOCATE  = 1,  // the driver consumed the fd
    VK_ENC_IMPORT_SUCCESS                = 2,
};

struct VkEncImportedImage {
    VkImage        image  = VK_NULL_HANDLE;    // valid iff SUCCESS
    VkDeviceMemory memory = VK_NULL_HANDLE;    // valid iff SUCCESS
    VkDeviceSize   allocationSize = 0;         // what was actually allocated
    VkEncExternalImageImportResult result =
        VK_ENC_IMPORT_FAILED_BEFORE_ALLOCATE;
    // SUCCESS-class memory-type telemetry (heuristic selections,
    // exporter-index overrides); the member wrapper folds these into the
    // session counters.
    uint32_t heuristicSelections = 0;
    uint32_t exporterOverrides   = 0;
};

// vkCreateImage + memory-type selection + vkAllocateMemory(import chain) +
// vkBindImageMemory, from |desc|'s TRUE properties. |desc| has already
// passed ValidateImageDescriptor, and under BORROW |osHandle| is already
// the library's private duplicate.
//
// fd ownership on exit -- the rule that the library closes anything it did
// NOT hand to the driver, on every exit path, applied literally:
//   * FAILED_BEFORE_ALLOCATE: the fd never reached the driver; THIS
//     function closes it before returning.
//   * FAILED_AFTER_ALLOCATE: the failed vkAllocateMemory consumed it, or
//     the allocated-then-unbindable VkDeviceMemory released it via the
//     cleanup vkFreeMemory. Nobody closes it again -- not this function,
//     not any caller.
//   * SUCCESS: the VkDeviceMemory owns it; vkFreeMemory is the release.
// On every exit the fd is consumed from the API caller's point of view
// (handlesConsumed stays VK_TRUE); the split only names which owner
// performs the close, and that owner is exactly one. Win32 handles are
// never closed here on any path, per the public header's rule.
VkVideoEncoderStatusCode VkEncImportExternalImage(
    const VulkanDeviceContext& vkDevCtx,
    const VkVideoEncoderExternalImageDescriptor& desc,
    uint64_t osHandle,
    VkEncImportedImage* outImport);

// ---------------------------------------------------------------------------
// The dma-buf import-ordinal guard's verdict, carried from the guard (which
// runs deep inside the import) out to RegisterImageResource, which is the
// only place that can hand it to a caller. The public carrier is
// VkVideoEncoderImportGuardInfo; this is the internal hop, kept off the
// public surface because it has no sType and is not ABI.
//
// THREAD-LOCAL, deliberately. The guard writes it on the thread running the
// import and RegisterImageResource reads it back on that same thread (class
// (b), submit-thread-affine), so two sessions registering concurrently cannot
// overwrite each other's verdict -- which a process global would let them do,
// silently, exactly when a consumer is trying to diagnose one of them.
struct VkEncImportOrdinalGuardReport {
    VkVideoEncoderImportGuardState state =
        VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED;
    // What this BUILD retains where the guard applies -- a constant, not an
    // outcome, so it is filled even on the paths the guard never evaluates.
    // 0 in a stock build, because the guard is disabled by default.
    uint32_t requestedCount = 0;
    uint32_t retainedCount  = 0;
    VkVideoEncoderStatusCode failureStatus = VK_VIDEO_ENCODER_STATUS_SUCCESS;
    int32_t  failureErrno = 0;
};

// Clear the calling thread's record and re-stamp requestedCount. Called at
// the top of every RegisterImageResource, so a registration that never
// reaches the import reports NOT_EVALUATED rather than the previous
// registration's answer.
void VkEncResetImportOrdinalGuardReport();

void VkEncGetImportOrdinalGuardReport(VkEncImportOrdinalGuardReport* outReport);

// ---------------------------------------------------------------------------
// Release of the dma-buf import-ordinal guard.
//
// The guard retains a small number of sacrificial dma-buf image imports for
// the LIFE OF THE DEVICE, so that every caller-visible import lands that many
// live-positions later. It is DISABLED BY DEFAULT -- the build count is 0,
// nothing is retained, this function finds nothing and returns 0. Everything
// below is about a build that enables it. "For the life of the device" cuts
// both ways, and this function is the second half of it:
//
//   * RELEASED TOO EARLY, the shift the count was chosen for is undone, and
//     undone SILENTLY: the freed position is handed straight to the next
//     caller import and nothing reports it. So the only correct call site is
//     one where no further import on |vkDevCtx| is possible at all:
//     VulkanVideoEncoderExtImpl::Deinitialize(), after the encoder has been
//     released and every registration retired.
//   * NEVER RELEASED, and vkDestroyDevice runs with those VkImage and
//     VkDeviceMemory objects still alive on the device
//     (VUID-vkDestroyDevice-device-05137).
//
// RETURNS the number of guard imports actually released for |vkDevCtx|'s
// device: 0 when the guard never ran on it (the disabled default, a device it
// does not apply to, no dma-buf import, kill switch set, or a device that
// never imported), and the full guard count when it did. A return value
// rather than only a log line, because an embedder can silence the library's
// narration.
//
// Idempotent and null-safe: the device's entry is taken off the registry
// before anything is destroyed, so a second call for the same device returns 0
// and destroys nothing, and a VK_NULL_HANDLE device returns 0.
uint32_t VkEncReleaseImportOrdinalGuard(const VulkanDeviceContext& vkDevCtx);

// ---------------------------------------------------------------------------
// Fault-injection seam for the release-obligation guard.
//
// That mitigation is only as good as its tests: every post-arm early return
// in SubmitRegisteredFrame owes the registration a release, and the sites
// that matter most -- a refused submit AFTER the reference is taken -- are
// reachable on demand only by making the submit backend fail, which no test
// against working hardware can do. So the backend is the seam: a null
// backend that stands in for the encoder on the one call the submit makes to
// it, while every gate, resolution step and bookkeeping path around it stays
// the production code. Null by default; a production session never installs
// one, so the production-path cost is a null pointer test on the gates that
// must admit a backend-less session.
//
// Build placement, stated honestly: the five seam functions below are
// compiled into the production library objects. The usual discipline for a
// seam -- a test-only build target no production target may depend on -- is
// not applied here; nothing in production references these symbols (only
// the library test TUs do), but that gate is convention, not the build
// system. Applying
// the testonly discipline means extracting the impl class definition into
// a src/-private header and moving these five definitions into a TU that
// only test targets compile, on both build systems; until then, this
// header's include list is the boundary to review.

class VulkanVideoEncoderExt;

struct VkEncNullBackendState {
    // What the submit backend answers. VK_SUCCESS enqueues the pending
    // entry through the same bookkeeping the real backend's success arm
    // runs; anything else is returned as that backend failure.
    VkResult submitResult = VK_SUCCESS;
};

// Install |state| (caller-owned; must outlive the encoder) on a freshly
// created, never-initialized encoder from CreateVulkanVideoEncoderExt -- no
// other object may be passed here. The session then reports initialized
// with no device, no worker threads and -- until VkEncPushCapture installs
// its capture source -- no VkVideoEncoder behind it:
// VK_IMAGE registration skips the device-touching view build (the import
// arms still refuse without a device, at validation) and submits terminate
// at the null backend. VK_ERROR_NOT_PERMITTED_KHR on a session that is
// already initialized: backends stand in for a real session, they never
// replace one.
VkResult VkEncInstallNullBackend(VulkanVideoEncoderExt* encoder,
                                 const VkEncNullBackendState* state);

// THE IMPORT CONTENT PROBE'S MEASUREMENT SEAM.
//
// Hands back the three plane means the probe measured for |resource|, and --
// the point of the seam -- lets a test INJECT a measurement without a device.
//
// WHY THIS EXISTS AND WHAT IT IS NOT. The probe's own capture needs a real
// dma-buf import, a real staging command buffer and a real fence; nothing
// device-free can produce one. Everything AFTER the measurement -- the
// predicate, the per-registration latch, the oldest-damaged-first report, the
// drain when the consumer retires the buffer, and the whole ext-side reporting
// path through GetCompletionInfo -- is pure host logic, and without this
// seam it would be exercisable only on a device that exhibits the import
// defect. An observable whose only test cannot fail is not an observable.
//
// It injects a MEASUREMENT, never a verdict: the state the caller then reads
// back is the production predicate's answer, not something the test chose.
// A |resource| that is not armed, or already scored, is a no-op -- the
// once-per-registration rule is the production rule and this does not get to
// bypass it.
//
// Returns VK_ERROR_NOT_PERMITTED_KHR when no registration ever asked for a
// content probe (nothing to inject into).
VkResult VkEncInjectImportContentMeasurement(VulkanVideoEncoderExt* encoder,
                                             VkVideoEncoderResource resource,
                                             uint32_t meanYQ8,
                                             uint32_t meanUQ8,
                                             uint32_t meanVQ8);

// The registration-slot facts the R-2 tests assert on. A dropped (or
// stranded) in-flight reference is observable only here: the public surface
// deliberately answers a stale id with RESOURCE_UNKNOWN whether the slot is
// retired, pinned, or gone.
struct VkEncResourceProbe {
    uint32_t inFlight = 0;
    VkBool32 live     = VK_FALSE;
    VkBool32 retired  = VK_FALSE;
    // The ROUTING DECISION, which is the whole point of the registration
    // gate. The public surface carries no accessor for it -- deliberately:
    // it is an implementation choice, not a contract -- and the only other
    // trace of it is a VkEncErr line that silenceStdio can null for a whole
    // session. Without this field a test can assert that a descriptor
    // REGISTERED but not what it registered AS, which makes "routes via the
    // compute filter" and "was accepted at all" one observation.
    //
    // The two view facts are reported beside it because the path is a
    // FUNCTION of them: reading only inputPath cannot distinguish "the
    // filter clause failed" from "the format clause did", and those are
    // different bugs.
    VkVideoEncoderExternalInputPath inputPath =
        VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    VkBool32 planeStorageViews = VK_FALSE;
    VkBool32 storageReadView    = VK_FALSE;
};

// Fill |outProbe| for the slot |resource| names. RESOURCE_UNKNOWN once the
// slot has been destroyed (its generation advanced) or never existed --
// which is itself the assertion that a deferred retirement completed.
VkVideoEncoderStatusCode VkEncProbeResource(VulkanVideoEncoderExt* encoder,
                                            VkVideoEncoderResource resource,
                                            VkEncResourceProbe* outProbe);

// Whether the session currently reports initialized. Deinitialize() -- the
// worker join on the destruction path -- is what flips it, so a release
// thunk that reads VK_FALSE here has proof the teardown already ran. This
// is what the destructor-order test asserts, because the thing the order
// actually protects (a worker still inside an invocation) needs a device
// to exist.
VkBool32 VkEncSessionInitialized(VulkanVideoEncoderExt* encoder);

// Raise the completion edge exactly as a capture push does -- counter,
// OS-event signal, serialized callback invocation -- from a plain test
// thread. This is what lets the trampoline stress test race ReadyThunk
// invocations against SetCompletionCallback replace/detach without a real
// encode session: the delivery threads the production edge runs on exist
// only with a device.
void VkEncFireCompletionEdge(VulkanVideoEncoderExt* encoder,
                             uint64_t frameId);

// Deliver a completion record for |frameId| into the session exactly as a
// completed encode would: pushed through the capture funnel on a source
// installed as the session's encoder, so the pop, the match against the
// pending set and the late-capture accounting all run the production drain
// under the production lock order. Null-backend sessions only
// (VK_ERROR_NOT_PERMITTED_KHR otherwise): injection stands in for a real
// session's completion path, it never runs beside one. The first push
// installs a device-free capture source behind the session -- the one
// departure from the no-VkVideoEncoder shape VkEncInstallNullBackend
// documents -- wired to the completion edge exactly as InitializeExt wires
// a real encoder.
VkResult VkEncPushCapture(VulkanVideoEncoderExt* encoder,
                          uint64_t frameId,
                          VkResult status);

// ---------------------------------------------------------------------------
// Sync-resolution observation seam.
//
// SubmitRegisteredFrame's chained-descriptor walk decides, PER DIRECTION,
// whether the submit is handed the caller's raw semaphore arrays or the ones
// the walk resolved. That decision is invisible from the public surface: the
// submit consumes the arrays and answers a status. So a null-backend session
// records what it was handed, and the three functions below set the chain up
// and read the record back -- with no device, no encoder and no queue.
//
// The rule these exist to pin: a chain naming only waits must leave the
// caller's signal list alone, and a chain naming only signals must leave the
// caller's wait list alone. Overriding both directions whenever either is
// resolved would silently zero the caller's signal semaphores on every
// acquire-fence-only frame.

// Register |semaphore| device-free and answer the id a FrameSyncDescriptor
// names it by. The public RegisterSemaphore cannot run here: it creates a
// timeline VkSemaphore and imports an OS handle into it, and a null-backend
// session has no device with which to do either -- so without this, a
// FrameSyncDescriptor on such a session can only ever resolve to nothing.
// |semaphore| is stored, compared and handed to the submit, never
// dereferenced; pass a distinct non-null sentinel per call.
//
// MUST be paired with VkEncUninstallTestSemaphore before the session is
// destroyed. Deinitialize destroys every still-live registry entry through the
// device dispatch table, which on a device-free session is unpopulated; the
// pairing is what keeps that loop from ever reaching one of these entries.
// VK_ERROR_NOT_PERMITTED_KHR unless the session is null-backend.
VkResult VkEncInstallTestSemaphore(VulkanVideoEncoderExt* encoder,
                                   VkSemaphore semaphore,
                                   VkVideoEncoderResource* outResource);

// Retire an id from VkEncInstallTestSemaphore with UnregisterSemaphore's slot
// hygiene -- handle nulled, live cleared, generation bumped -- and without the
// driver destroy the public unregister performs. Null-backend sessions only.
VkVideoEncoderStatusCode VkEncUninstallTestSemaphore(
    VulkanVideoEncoderExt* encoder, VkVideoEncoderResource resource);

// How many entries of each array VkEncSubmitSyncProbe carries. The counts it
// reports are NOT clamped to this, so a truncated record is always
// distinguishable from a short list.
enum { kVkEncSubmitSyncProbeCapacity = 8 };

// The two arrays the most recent submit on a null-backend session was handed,
// AFTER the walk applied its per-direction override. The record is taken at
// the null backend: below every gate and below the walk, above the two
// SetExternalInputFrame* call sites -- and those forward these same
// VkVideoEncodeInputFrame fields verbatim, so what is recorded is what either
// site would submit.
struct VkEncSubmitSyncProbe {
    // VK_FALSE until a submit reaches the null backend.
    VkBool32    recorded    = VK_FALSE;
    uint32_t    waitCount   = 0;
    uint32_t    signalCount = 0;
    VkSemaphore waitSemaphores[kVkEncSubmitSyncProbeCapacity]   = {};
    uint64_t    waitValues[kVkEncSubmitSyncProbeCapacity]       = {};
    VkSemaphore signalSemaphores[kVkEncSubmitSyncProbeCapacity] = {};
    uint64_t    signalValues[kVkEncSubmitSyncProbeCapacity]     = {};
};

VkVideoEncoderStatusCode VkEncProbeLastSubmitSync(
    VulkanVideoEncoderExt* encoder, VkEncSubmitSyncProbe* outProbe);

#endif  // VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_
