/*
 * Internal to the encoder library and its tests. NOT part of the public ABI:
 * nothing here may be relied on by a consumer. The observation structs below
 * carry a structure type because they ride the public pNext chains; that
 * type is an internal detail like the rest of this header.
 *
 * THAT IS A BUILD FACT, NOT A REQUEST. This file lives outside the include
 * directory the encoder library exports, and it is not installed. A consumer
 * that links the library, or that builds against an install prefix, does not
 * have this header on its include path and cannot include it by name. The
 * library and the in-tree tests that need it name this directory explicitly;
 * that naming is what distinguishes an internal consumer from a client.
 *
 * Two layers of machinery, described below, that make
 * "accepted and ignored" structurally detectable rather than a thing anyone
 * has to remember.
 *
 * Layer 1 is the field table below. Every field of VkVideoEncoderConfig
 * appears exactly once with a disposition saying what happens to it. Each
 * entry carries an offsetof assertion, so renaming or removing a field fails
 * the build here, and each carries the SIZE and the ALIGNMENT of the field it
 * names so that the rows can be checked against the struct as a whole and not
 * only against one another.
 *
 * A FIELD WITH NO ROW is the case the offsetof assertions cannot see: those
 * assertions are taken FROM rows, so a field nobody wrote a row for is never
 * named by one. The sizeof assertion on the struct, and the closing-member
 * pins beside it, fail when an addition changes the size or slides a run --
 * but both are PROMPTS rather than proofs. They fail where the pin is, not at
 * this table, and a field whose pin was updated and whose row was not written
 * still compiles. That is how inputColorModel reached this struct with no row
 * here, and nothing failed.
 *
 * WHAT CLOSES IT is the tiling check, which lives in this tree rather than in
 * a consumer: vk_video_encoder/test/encoder-ext-filter, run by ctest as
 * EncoderExtInputFormatTaxonomy. Sorted by offset, the rows must lie end to
 * end across VkVideoEncoderConfig -- no row starting inside its predecessor, a
 * gap before a row legal only while it is STRICTLY narrower than that row's
 * alignment, the last row closing the struct, and the gaps totalling
 * kVkEncCfgPaddingBytes. Between them those fail for the removal of ANY row in
 * this table.
 *
 * THE ONE CASE NONE OF THEM SEES is a field added into padding that already
 * exists: it moves neither the struct's size nor any offset, so there is
 * nothing for an arithmetic check to count. Classifying a new field remains a
 * decision an author makes; these checks are what put the question in front of
 * them.
 *
 * Layer 3 is the binder conformance suite, which drives
 * VkEncBuildAndProbeConfig per codec arm and asserts effect or explicit
 * rejection for every BOUND field except outputPath, which has no probe
 * projection: its binding is a conditional file-open, and a consumer that
 * captures in memory nulls the field. Those assertions are written by hand;
 * no test walks this table pairing dispositions with effects, so a new
 * BOUND field is covered only once its author adds the assertion. The tests
 * that DO iterate the table are the field-table cases of the taxonomy suite
 * named above; they check its SHAPE -- exactly-once classification, in-struct
 * offsets, and the tiling -- not field effect. A consumer of this header may
 * carry a test of that shape too, but a guarantee this header states has to
 * be one this library can run, so the in-tree cases are what it cites.
 * The binder is exposed as a free function because it touches
 * no member state, so the suite drives it with no Vulkan device at all.
 */

#ifndef VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_
#define VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_

#include <cstddef>
#include <cstdint>

// The public header, which every target allowed to include THIS header
// already has on its include path: the public directory is what the library
// target exports, and this directory is named only by the library and by the
// in-tree tests that reach in here. Deliberately does NOT reach into the
// library's private headers -- see VkEncBoundConfigProbe.
#include "vulkan_video_encoder_ext.h"

// ---------------------------------------------------------------------------
// INTERNAL OBSERVATION STRUCTS
//
// What the library did with a frame, and what a dma-buf import produced.
// None of it is part of the client contract: a caller encodes without naming
// any of these types, and the routing decisions and driver mitigations they
// report are implementation choices the library is free to change.
//
// They ride the public pNext chains -- VkVideoEncoderCompletionInfo::pNext on
// GetCompletionInfo, and VkVideoEncoderStatus::pNext on
// RegisterImageResource -- so each carries a structure type, taken from a
// band of the public numbering that is assigned to these and never reused.
// The public chain rules apply unchanged: value-initialize the struct so it
// self-stamps, chain at most one link of each type, and expect an unknown or
// repeated link to be refused rather than ignored.
// ---------------------------------------------------------------------------

constexpr VkVideoEncoderStructureType
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO =
        (VkVideoEncoderStructureType)0x56450019;
constexpr VkVideoEncoderStructureType
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO =
        (VkVideoEncoderStructureType)0x5645001A;
constexpr VkVideoEncoderStructureType
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_STAGED_SUBMIT_INFO =
        (VkVideoEncoderStructureType)0x5645001B;
constexpr VkVideoEncoderStructureType
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO =
        (VkVideoEncoderStructureType)0x5645001C;
constexpr VkVideoEncoderStructureType
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO =
        (VkVideoEncoderStructureType)0x5645001D;

// Which preprocess conversion the library built for a session.
enum VkVideoEncoderFilterType {
    VK_VIDEO_ENCODER_FILTER_TYPE_NONE = 0,
    // Any YCbCr -> YCbCr conversion, including 3-plane I420 -> 2-plane NV12
    // (plane-count and bit-depth conversion) and the identity copy.
    VK_VIDEO_ENCODER_FILTER_TYPE_YCBCR_COPY = 1,
    VK_VIDEO_ENCODER_FILTER_TYPE_RGBA_TO_YCBCR = 2,
    VK_VIDEO_ENCODER_FILTER_TYPE_YCBCR_TO_RGBA = 3,
};

// Filter dispatch: chain onto VkVideoEncoderCompletionInfo::pNext.
//
// Whether a preprocess conversion ran, and how much of the session took it.
// The session's input format decides whether a filter is BUILT; the route is
// chosen per frame, so a session that has one can still send some or all
// frames down the staging copy.
//
// filterDispatchCount and stagedCopyCount are the two arms of one per-frame
// decision and never both count the same frame, so they are read directly
// rather than by subtracting from a total:
//
//   filterCreated == VK_FALSE          no filter on this session
//   created, dispatch == 0, copy > 0   configured; every frame copied
//   created, dispatch > 0, copy == 0   the filter is the path
//   dispatch == 0 && copy == 0         nothing reached staging -- a
//                                      zero-copy route bypasses it -- or
//                                      there is no snapshot to take
//
// All four fields are filled only while the session still holds an encoder.
// Read them before Flush() and before the last reference to the encoder goes;
// after either, the snapshot is filterCreated = VK_FALSE with both counts 0,
// which is the last row of the table and is honest but indistinguishable from
// a session that has no filter and never staged a frame.
//
// Counts are cumulative for the life of the encoder and never reset.
struct VkVideoEncoderFilterInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO;
    const void*                 pNext = nullptr;

    VkBool32                 filterCreated;  // a filter OBJECT exists
    VkVideoEncoderFilterType filterType;     // which conversion was built
    uint64_t filterDispatchCount;  // filter command buffers RECORDED
    uint64_t stagedCopyCount;      // frames that took the staging copy arm
};

// Staging acquire program: chain onto VkVideoEncoderCompletionInfo::pNext.
//
// Which of the two staging-acquire programs a registered external frame took
// -- the VK_QUEUE_FAMILY_FOREIGN_EXT ownership acquire, or the local
// HOST|TRANSFER availability barrier. The library picks one from the declared
// residency and layout. Both are self-consistent, both leave the image in the
// same layout, and neither violates a core VUID, so the counts are what tells
// them apart.
//
// The two counts are the two arms of one per-frame decision and never both
// count the same frame:
//
//   foreign > 0, local == 0    every staged frame took the FOREIGN acquire
//   foreign == 0, local > 0    every staged frame took the local restore
//   both 0                     nothing reached the staging tier -- a direct
//                              zero-copy registration bypasses it -- or
//                              there is no snapshot to take
//
// COUNTED AT THE ROUTING DECISION, NOT AT THE BARRIER: once for each frame
// whose staging barrier program was chosen and recorded, on either arm. A
// count taken inside the two arms' own release and handback pairs would
// undercount a session that mixes filtered and copied frames, and a superset
// counter can make a live tier read as dead.
//
// EXTERNAL INPUT ONLY. The library's file-input lane declares no residency
// and is not counted here; counting it would make localAcquireCount equal the
// frame count on a session that registered nothing.
//
// Filled only while the session still holds an encoder, so read before
// Flush() and before the last encoder reference goes. DrainPendingFrames()
// does not clear it and is the right place to read after: it joins the
// encoder threads, so every submitted frame has been routed by the time it
// returns.
//
// Counts are cumulative for the life of the encoder and never reset.
struct VkVideoEncoderInputResidencyInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO;
    const void*                 pNext = nullptr;

    // Frames whose staging acquire named VK_QUEUE_FAMILY_FOREIGN_EXT as its
    // source family (an external allocator or queue family owns the memory).
    uint64_t foreignAcquireCount;
    // Frames whose staging acquire was a same-family availability barrier
    // and whose handback RESTORED the layout instead of releasing ownership.
    // Its scopes follow the arm that ran -- transfer for the staging copy,
    // compute for the filter.
    uint64_t localAcquireCount;
};

// Staged-input submit engine: chain onto VkVideoEncoderCompletionInfo::pNext.
//
// Which queue family the staged input work is recorded and submitted on --
// the acquire, the copy or filter dispatch, the release and the submit alike.
// Both arms of the staging path take their command buffer from one pool, the
// pool is created on the compute family whenever the session has a preprocess
// filter, and a command buffer may only be submitted to a queue of its pool's
// family (VUID-vkQueueSubmit2-commandBuffer-03874). So a frame that
// dispatches no filter still stages on the compute family the moment the
// session has one, without that frame's own format having changed.
//
// The family is more than scheduling. A driver may lose the device executing
// a queue-family RELEASE to VK_QUEUE_FAMILY_FOREIGN_EXT of an image created
// with VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR or ..._DPB_BIT_KHR when the
// barrier is recorded off the graphics or optical-flow families; the usage
// bit is the gate there, not the family alone. The staged copy arm releases
// the CALLER's imported image, whose usage the caller declared, so safety is
// a joint property of this family and that usage and both have to be read.
// Neither family is a validation error, so reporting is the only way to see
// which one ran.
//
// SESSION-CONSTANT, and filled from the same accessors the staging and submit
// sites read rather than re-derived, so a barrier site and a submit site that
// name different families are observable here. Valid before any frame stages.
//
// Filled only while the session still holds an encoder; afterwards it reports
// submitTypeQueueFlags 0 and VK_QUEUE_FAMILY_IGNORED.
struct VkVideoEncoderStagedSubmitInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_STAGED_SUBMIT_INFO;
    const void*                 pNext = nullptr;

    // The raw VK_QUEUE_* bit the staged-input batch is submitted with
    // (VK_QUEUE_COMPUTE_BIT 0x2, VK_QUEUE_TRANSFER_BIT 0x4,
    // VK_QUEUE_VIDEO_ENCODE_BIT_KHR 0x40). Reported as the flag rather than as
    // a library enum so no translation table can drift from the submitted
    // value. 0 means there is no session.
    uint32_t submitTypeQueueFlags;
    // The queue-family index that flag resolves to on this device, i.e. the
    // family named as the DESTINATION of the staged FOREIGN acquire and as the
    // SOURCE of the staged FOREIGN release. VK_QUEUE_FAMILY_IGNORED means
    // there is no session.
    uint32_t queueFamilyIndex;
};

// ---------------------------------------------------------------------------
// The dma-buf import-ordinal guard, and how to read its report.
//
// The guard mitigates a class of dma-buf import defect in which the imported
// image is bound to memory the exported buffer's contents never reach. It
// holds a fixed number of sacrificial imports on the device ahead of any
// caller-visible one, so caller imports land at later live-positions. That is
// a change of POSITION, not a repair: an import that lands damaged is still
// damaged, and what the buffer actually holds is the question
// VkVideoEncoderImportContentInfo below answers.
//
// THE GUARD IS DISABLED BY DEFAULT: a stock build retains nothing and
// reports DISABLED for every dma-buf import inside the guard's scope. Outside
// that scope the out-of-scope verdict is reported instead, so read |state|
// rather than assuming DISABLED. VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD in
// the environment disables the guard in a build that enables it.
//
// WHERE TO CHAIN VkVideoEncoderImportGuardInfo.
//
//   * VkVideoEncoderStatus::pNext on RegisterImageResource -- the verdict for
//     THAT registration, delivered by the call that ran the guard. This is
//     the one to assert on.
//   * VkVideoEncoderCompletionInfo::pNext on GetCompletionInfo -- the most
//     recent verdict from a registration the guard evaluated, readable at any
//     time. A registration outside the guard's scope leaves this snapshot
//     unchanged, so it cannot erase the answer a dma-buf registration
//     established. It reads NOT_EVALUATED before the first evaluated one.
//
// THE STATES. |state| is the verdict; requestedCount and retainedCount are
// the arithmetic behind it. requestedCount is a BUILD CONSTANT, filled on
// every path, so a reader compares against it rather than hard-coding it.
//
//   STATE            MEANING                        WHAT TO DO
//   ---------------  -----------------------------  ---------------------
//   NOT_EVALUATED    The guard did not run: a       Nothing to read. Also
//                    VK_IMAGE registration, or      the zero value.
//                    one refused before the
//                    import.
//   NOT_APPLICABLE   Out of the guard's scope:      Nothing to read.
//                    not a DMA_BUF handle, or no
//                    device to hold a position on.
//   NOT_NVIDIA       Not a device the guard         Nothing to read.
//                    applies to.
//   DISABLED         Off deliberately: this build   Nothing to read. The
//                    retains 0, or the kill         stock build's answer.
//                    switch is set.
//   COMPLETE         retainedCount ==               Caller imports land at
//                    requestedCount, on a build     live-position
//                    that asked for a non-zero      requestedCount + 1 or
//                    count.                         later.
//   INCOMPLETE       retainedCount <                Read failureStatus and
//                    requestedCount: the shift      failureErrno; also in
//                    asked for did not happen.      the diagnostic channel.
// ---------------------------------------------------------------------------
typedef enum VkVideoEncoderImportGuardState {
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED  = 0,
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_APPLICABLE = 1,
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_NVIDIA     = 2,
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_DISABLED       = 3,
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_COMPLETE       = 4,
    VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_INCOMPLETE     = 5,
} VkVideoEncoderImportGuardState;

typedef struct VkVideoEncoderImportGuardInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO;
    const void*                 pNext = nullptr;  // MUST be NULL

    VkVideoEncoderImportGuardState state =
        VK_VIDEO_ENCODER_IMPORT_GUARD_STATE_NOT_EVALUATED;          // OUT
    // Sacrificial imports this build retains where the guard applies.
    // A build constant, filled on every path. 0 in a stock build: the
    // guard is disabled by default.
    uint32_t requestedCount = 0;                                    // OUT
    // Sacrificial imports actually live on the device right now.
    uint32_t retainedCount  = 0;                                    // OUT
    // INCOMPLETE only: what refused the sacrificial import.
    // ERROR_IMPORT_FAILED with a non-zero failureErrno means dup(2)
    // failed; any other value is the import's own status.
    VkVideoEncoderStatusCode failureStatus =
        VK_VIDEO_ENCODER_STATUS_SUCCESS;                            // OUT
    int32_t failureErrno = 0;                                       // OUT
} VkVideoEncoderImportGuardInfo;

// ---------------------------------------------------------------------------
// The imported-buffer content probe, and how to read its verdict.
//
// A dma-buf import can come back bound to memory the producer's writes never
// reach. VkVideoEncoderImportGuardInfo above reports what a mitigation did;
// this reports what the imported buffer actually CONTAINS.
//
// It is a content observation of the FIRST frame each registration serves,
// taken at the staged copy. A registration that would otherwise encode
// directly sends that one frame through the staged path and every later
// frame direct, so the cost is one detoured frame per registration. It is
// not a repair and not a prediction, and it cannot see a buffer that has not
// yet carried a frame. The reaction to a damaged verdict is to stop using
// that registration.
//
// THE PREDICATE. Let meanY, meanU and meanV be the plane means of the
// imported frame, in 0..255:
//
//   DAMAGED_ALL     <=  (meanY < 2) && (meanU < 2) && (meanV < 2)
//   DAMAGED_CHROMA  <=  (meanY >= 2) && ((meanU < 2) || (meanV < 2))
//   CLEAN           <=  neither
//
// Luma and chroma are scored together rather than chroma alone, so an
// all-zero buffer and a chroma-zeroed one stay distinguishable.
//
// ZEROED IS NOT BLACK, which is what makes the test sound: legal black in
// NV12 is Y=16 (0 in full range) with U=V=128, so a zero chroma plane is a
// value no correct encoder input carries. The false-positive budget is a
// frame that is deliberately all-zero in every plane, which from inside the
// library is indistinguishable from the defect; the cost is one frame per
// registration, and the reaction to it is not destructive.
//
// WHEN THE VERDICT EXISTS. Not at registration -- the producer has written
// nothing yet, so there is nothing to score. The registration echo reports
// ARMED or NOT_APPLICABLE; the verdict arrives on a later GetCompletionInfo
// snapshot, once the first frame of that registration has been submitted and
// its fence waited. Frames submitted in the meantime encode against the
// buffer and cannot be recalled, because the library does not recall
// submitted GPU work (see CancelFrame): roughly one pipeline depth of frames
// is the price of scoring content that only exists once it is written.
//
// WHERE TO CHAIN IT. Exactly where VkVideoEncoderImportGuardInfo chains, and
// the two are independent -- either, both, or neither.
//
//   * VkVideoEncoderStatus::pNext on RegisterImageResource. CHAINING IT HERE
//     IS THE OPT-IN: a registration whose status carries this struct arms the
//     probe; one that does not is never probed and pays nothing. There is no
//     environment variable and no build flag. The value read back on a
//     successful registration is ARMED or NOT_APPLICABLE, and
//     NOT_EVALUATED on a registration this call refused -- never a verdict.
//   * VkVideoEncoderCompletionInfo::pNext on GetCompletionInfo -- the verdict
//     channel, readable at any time from any thread. It reports the OLDEST
//     still-registered DAMAGED_* registration, so retiring that one exposes
//     the next on the following poll and not reacting re-reports the same
//     one: idempotent either way, and no verdict is lost between polls. With
//     none damaged it reports the most recent CLEAN verdict, or NOT_EVALUATED
//     before the first frame is scored.
//
// THE STATES.
//
//   STATE            MEANING                        WHAT TO DO
//   ---------------  -----------------------------  ---------------------
//   NOT_EVALUATED    No verdict yet: nothing        Poll again later. Also
//                    armed, or nothing armed has    the zero value.
//                    completed a frame.
//   NOT_APPLICABLE   This registration cannot be    No verdict will come.
//                    scored.
//   ARMED            Set up, waiting for a frame.   Expect a verdict on a
//                    A registration echo reports    later snapshot.
//   CLEAN            Scored; the predicate did      Keep using the
//                    not fire.                      registration.
//   DAMAGED_CHROMA   Scored: chroma dead, luma      Stop using the
//                    alive.                         registration.
//   DAMAGED_ALL      Scored: every plane dead.      Stop using it.
//
// NOT_APPLICABLE is usually decided at registration and arrives in the echo:
// the registration is FILTER-routed, which is a storage read and never a
// copy; the import carries no TRANSFER_SRC, so no copy may legally be
// recorded out of it; or the format is not 8-bit 2-plane 420, which the
// predicate needs in order to have a Y, a U and a V to score. It can also
// be latched on the first capture attempt -- the extent is degenerate, or
// the capture pool cannot serve that format and extent. An ARMED echo is
// therefore a verdict PENDING and not a verdict promised;
// armedRegistrationCount tells one still in flight from one that will never
// arrive.
//
// probeGeneration is a non-zero BUILD CONSTANT
// (VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION) stamped on every path,
// so probeGeneration != 0 is proof the library wrote the struct.
//
// meanY / meanU / meanV are the plane means in Q8 FIXED POINT -- the 0..255
// mean times 256, so 128.0 reads as 32768 and the predicate's "< 2" is
// "< 512". Integers rather than floats because this struct crosses a process
// boundary. They are filled on CLEAN and DAMAGED_* alike, so a reader can
// log why.
// ---------------------------------------------------------------------------

// Non-zero by contract: a non-zero probeGeneration is what proves the
// library wrote the struct at all. Bump it if the predicate or the sampling
// changes in a way that makes old and new verdicts non-comparable.
#define VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION 1u

// The predicate's threshold, in the Q8 units meanY/meanU/meanV carry: a plane
// mean strictly below 2.0/255. Named rather than open-coded because the
// library's scorer and every assertion on it have to agree.
#define VK_VIDEO_ENCODER_IMPORT_CONTENT_DEAD_PLANE_MEAN_Q8 512u

typedef enum VkVideoEncoderImportContentState {
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED  = 0,
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE = 1,
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED          = 2,
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_CLEAN          = 3,
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_CHROMA = 4,
    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_ALL    = 5,
} VkVideoEncoderImportContentState;

typedef struct VkVideoEncoderImportContentInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO;
    const void*                 pNext = nullptr;  // MUST be NULL

    // Which registration this verdict belongs to.
    // VK_VIDEO_ENCODER_RESOURCE_NULL when there is no verdict
    // (NOT_EVALUATED), and on the registration echo, where the resource id is
    // the call's own return value.
    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;   // OUT
    VkVideoEncoderImportContentState state =
        VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED;            // OUT
    // Non-zero build constant, stamped on every path. The writer proof.
    uint32_t probeGeneration = 0;                                       // OUT
    // Q8 fixed point: the 0..255 plane mean times 256.
    uint32_t meanY = 0;                                                 // OUT
    uint32_t meanU = 0;                                                 // OUT
    uint32_t meanV = 0;                                                 // OUT
    // Session totals: registrations that reached a CLEAN or DAMAGED_*
    // verdict, and how many of those were DAMAGED_*.
    uint32_t probedRegistrationCount = 0;                               // OUT
    uint32_t damagedRegistrationCount = 0;                              // OUT
    // REGISTRATIONS STILL WAITING FOR A VERDICT -- armed, not yet scored.
    //
    // READ THIS BEFORE BELIEVING damagedRegistrationCount == 0. The two
    // counts above cannot distinguish "every buffer was probed and every one
    // was clean" from "nothing was ever probed", because both report
    // probed=0 damaged=0 when no capture ever ran. This field is what tells
    // them apart: non-zero at the end of a session means that many buffers
    // were promised a verdict and never got one, so the absence of damage
    // reports is an absence of MEASUREMENT, not an absence of damage.
    //
    // Expected to be non-zero TRANSIENTLY -- a registration is armed at
    // import and scored a frame or two later, so a mid-session poll legit-
    // imately catches buffers in flight. It is a session that ENDS with this
    // non-zero, or a long-running session where it never falls, that means
    // the capture site is not being reached.
    uint32_t armedRegistrationCount = 0;                                // OUT
} VkVideoEncoderImportContentInfo;

// Disposition of a public config field.
//
//   BOUND    -- forwarded into EncoderConfig by the binder. The binder suite
//               asserts the landing for every BOUND field except outputPath
//               (no probe projection; see the file comment above).
//   SESSION  -- consumed by the ext layer itself (device/instance selection,
//               stdio silencing, codec dispatch). Never reaches EncoderConfig,
//               and must not: it shapes the session, not the encode.
//   ABI_GATE -- consumed by the versioning gate before anything else runs.
//   VALIDATED-- read as a DECLARATION and checked against the rest of the
//               config; refused at init when it cannot be honoured. Reaches
//               EncoderConfig nowhere, because there is nothing to forward:
//               the field states a requirement, it does not set a knob.
enum VkVideoEncoderConfigFieldDisposition {
    VK_ENC_FIELD_BOUND = 0,
    VK_ENC_FIELD_SESSION,
    VK_ENC_FIELD_ABI_GATE,
    // NO "REJECTED" DISPOSITION, AND ITS ABSENCE IS THE DECISION. There was
    // one, with a single definition and no row anywhere in the table below:
    // it advertised a class -- "a non-default value is refused at init because
    // honouring it is impossible in THIS BUILD" -- that this config surface
    // does not contain. Every refusal the surface makes is either a VALIDATED
    // declaration that cannot be honoured or an ABI_GATE, and both are
    // properties of the config rather than of the build. Deleted rather than
    // given a row: inventing a member to justify an enumerant is how a table
    // stops describing the thing it tables.
    VK_ENC_FIELD_VALIDATED,
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
    X(inputFormat,              BOUND,    "validated, then cfg->input.bpp, " \
                                          "cfg->input.chromaSubsampling "    \
                                          "and cfg->input.numPlanes -- the " \
                                          "subsampling is what makes a "     \
                                          "4:4:4 or 4:2:2 encode profile "   \
                                          "reachable at all")                \
    X(inputColorModel,          BOUND,    "with inputFormat, the pair a "      \
                                          "session is declared in; "           \
                                          "resolved to "                       \
                                          "cfg->input.colorSpace, which "      \
                                          "selects the input plane count "     \
                                          "and subsampling. A "                \
                                          "declaration the format cannot "     \
                                          "carry is refused at init")          \
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
    X(videoFullRange,           BOUND,    "cfg->video_full_range_flag; also "  \
                                "VALIDATED against a chained "                 \
                                "VkVideoEncoderInputColourInfo::inputRange -- " \
                                "VK_TRUE over a LIMITED Y'CbCr input is a "     \
                                "range conversion this library does not "       \
                                "perform and is refused at init")               \
    X(inputTransferCharacteristics, VALIDATED,                                 \
                                "checked against transferCharacteristics; a "  \
                                "declared mismatch is refused at init. "       \
                                "Reaches EncoderConfig nowhere: this library " \
                                "applies no transfer function, so there is "   \
                                "nothing to forward")                          \
    X(deviceId,                 SESSION,  "physical-device selection")         \
    X(gpuUUID,                  SESSION,  "physical-device selection")         \
    X(outputPath,              BOUND,    "cfg->outputFileHandler")             \
    X(verbose,                  BOUND,    "cfg->verbose")                      \
    X(validate,                 BOUND,    "cfg->validate")                     \
    X(disableFileOutput,        BOUND,    "cfg->disableFileOutput")            \
    X(silenceStdio,             SESSION,  "VkEncoderStdioSilenceScope")        \
    X(externalInstance,         SESSION,  "caller-supplied instance")          \
    X(externalPhysicalDevice,   SESSION,  "caller-supplied physical device")   \
    X(externalDevice,           SESSION,  "caller-supplied logical device")    \
    X(externalEncodeQueueFamilyIndex,  SESSION,                                \
                                "honoured only with externalDevice")           \
    X(externalComputeQueueFamilyIndex, SESSION,                                \
                                "honoured only with externalDevice")

// Stable index per field. No test iterates by these indices today; the one
// live consumer is kVkEncCfgFieldCount, which pins the table length (the
// static_assert below, re-checked at run time by the field-table cases in
// vk_video_encoder/test/encoder-ext-filter).
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
    // The EXTENT of the field this row names. An offset alone says where a row
    // starts and nothing about what it covers, so offsets alone can be checked
    // only against each other; with the extent, the rows can be laid end to
    // end and compared against the struct they claim to describe.
    //
    // DERIVED from the member, never written down beside it: a hand-copied
    // width is one more thing that can go stale, and a stale one would make
    // the tiling check agree with a layout the compiler does not have.
    size_t                                  size;
    size_t                                  align;
};

// Every field's existence is asserted by taking its offset: a rename or a
// removal stops compiling here, which is the point.
#define VK_ENC_FIELD_INFO(field, disposition, note)                            \
    {#field, VK_ENC_FIELD_##disposition, note,                                 \
     offsetof(VkVideoEncoderConfig, field),                                    \
     sizeof(VkVideoEncoderConfig::field),                                      \
     alignof(decltype(VkVideoEncoderConfig::field))},
static const VkVideoEncoderConfigFieldInfo kVkVideoEncoderConfigFields[] = {
    VK_VIDEO_ENCODER_CONFIG_FIELDS(VK_ENC_FIELD_INFO)
};
#undef VK_ENC_FIELD_INFO

static_assert(sizeof(kVkVideoEncoderConfigFields) /
                      sizeof(kVkVideoEncoderConfigFields[0]) ==
                  (size_t)kVkEncCfgFieldCount,
              "field table and field enum disagree");

// The bytes of VkVideoEncoderConfig that no field occupies: alignment padding
// between the rows above, once they are laid out in offset order. Four gaps
// carry all of it today -- 4 before pNext, 1 before videoFullRange, 3 before
// deviceId, 4 before outputPath.
//
// WHY A WRITTEN-DOWN NUMBER, as the struct's size is. The per-gap rule on its
// own -- a gap is legal while it is narrower than the alignment of the member
// that follows it -- still passes when the removed row sat in front of a
// WIDELY aligned successor, because the bytes it freed fit inside slack that
// successor already had. Two rows in this table are of exactly that shape:
// silenceStdio, four bytes ahead of an eight-aligned pointer, and
// matrixCoefficients, one byte ahead of a four-aligned VkBool32. Pinning the
// total is what catches those two -- the freed bytes have to surface
// somewhere, and once the total is pinned, here is where they surface.
//
// Move it only alongside the layout change that made it true.
constexpr size_t kVkEncCfgPaddingBytes = 12;

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
    // Chroma siting, as the H.26x VUI will carry it. Projected because it is
    // the ONLY observable of the preprocess filter's 2x2 box average outside
    // a decoded picture. "Present" and "type" have to be readable
    // separately, or a raised flag advertising type 0 looks identical to no
    // signal at all.
    uint32_t chromaLocInfoPresent;
    uint32_t chromaSampleLocType;
    // The INPUT side, as the chained VkVideoEncoderInputColourInfo landed in
    // EncoderConfig. It reaches the config through a pNext walk rather than a
    // config field, so nothing in the flat field table above can see whether
    // it bound. inputColourChainPresent is separate from the value fields for
    // the same reason av1ColorConfigPresent is separate from
    // av1ColorDescriptionPresent: 0 is UNDECLARED on every axis, so no value
    // field can tell "absent" from "present and zero".
    uint32_t inputColourChainPresent;
    uint32_t inputColourPrimaries;
    uint32_t inputTransferCharacteristics;
    uint32_t inputMatrixCoefficients;
    uint32_t inputRange;
    uint32_t verbose;
    uint32_t validate;
    uint32_t disableFileOutput;
    // EncoderConfig::enablePreprocessComputeFilter, not a public config
    // field: it is where the library's own preprocess-conversion decision
    // lands. Projected so that decision is assertable in BOTH directions --
    // a directly encodable input must write 0 here rather than inheriting
    // EncoderConfig's default of true (nothing else in the probe would
    // notice a filter object created behind the caller's back), and an input
    // that is encodable only after a conversion must write 1.
    uint32_t preprocessComputeFilter;
    // EncoderConfig::input.numPlanes. EncoderConfig does not store the input
    // format: it RECONSTRUCTS input.vkFormat from subsampling, bit depth and
    // this count. Left unwritten the count inherits EncoderConfig's default of
    // 3, which would describe every session's input as 3-plane I420.
    //
    // THIS IS NOT THE ONLY ROUTE TO "inputFormat WAS BOUND": inputVkFormat below
    // projects the reconstruction itself. The
    // count is still projected, and separately, because the two answer
    // different questions: this one is an INPUT to the reverse derivation and
    // that one is its OUTPUT, and a test that reads only the output cannot say
    // which of the three terms was wrong when it disagrees.
    uint32_t inputNumPlanes;
    // EncoderConfig::input.chromaSubsampling, the other half of what
    // inputFormat is read for. Projected for the same reason as the plane
    // count: the format is not stored, so the derivation is only assertable
    // through what it wrote. It is the field a 4:4:4 or 4:2:2 input has to
    // change -- left at its 4:2:0 default, a 4:4:4 request encodes as 4:2:0
    // and reports success -- and it is what the codec arm derives the encode
    // profile from. Carries the VkVideoChromaSubsamplingFlagBitsKHR value.
    uint32_t inputChromaSubsampling;
    // EncoderConfig::input.vkFormat AS IT STANDS AFTER InitializeParameters,
    // which is the OUTPUT of the reverse derivation and the one quantity that
    // says whether the library's two derivations of the input's identity
    // agree.
    //
    // THERE ARE TWO OF THEM, IN OPPOSITE DIRECTIONS. The binder derives
    // (chroma subsampling, bit depth, plane count) from the caller's
    // VkFormat; EncoderInputImageParameters::VerifyInputs() reconstructs a
    // VkFormat from those same three. The reverse one is LOAD-BEARING and
    // cannot be deleted: the packed-alias arm deliberately leaves vkFormat
    // unwritten so the reconstruction supplies it, which is the only route by
    // which AYUV and Y410 are nameable at all. So the two have to agree, and
    // this is what a test reads to say that they did.
    //
    // ON THE RGBA LANE THE REVERSE DERIVATION DOES NOT RUN -- VerifyInputs
    // carries the caller's format through, because CodecGetVkFormat spells no
    // RGB layout -- so this field projects the carry-through there. It is the
    // same proposition either way: the config's idea of the input format is
    // the caller's.
    uint32_t inputVkFormat;
    // THE OTHER SIDE OF THE SAME BOUNDARY: EncoderConfig's encode-side
    // geometry, which describes the BITSTREAM where the input fields above
    // describe the caller's buffer.
    //
    // WHY BOTH SIDES ARE PROJECTED WHEN ONE WRITER MAKES THEM EQUAL. They are
    // separate fields precisely so that the encode value can differ from the
    // input value -- a chroma resampler or a device-driven depth downgrade is
    // what would make them -- and every codec arm's profile derivation and
    // every syntax element that states the coded format is a function of THIS
    // side. A test that could see only one side could not say which side an
    // arm had read, which is how three arms came to read different ones.
    uint32_t encodeChromaSubsampling;
    uint32_t encodeBitDepthLuma;
    uint32_t encodeBitDepthChroma;

    // Per-codec-arm effect projections, run PER CODEC ARM. A projection
    // that stops at the shared EncoderConfig members cannot see
    // codec-conditional consumption: a field can reach the base config and
    // still have no effect on the arm that encodes. These project what each
    // arm actually hands the driver.
    // Zero when the arm was not exercised.
    //
    // H.26x: the rate-control layer info the codec arm builds. useMinQp /
    // useMaxQp are what make the clamp values legally visible to the driver.
    // What the arm's InitVuiParameters() ACTUALLY produced, as opposed to
    // what the shared EncoderConfig holds. An arm may write a VUI field from
    // the config and then overwrite it before the parameter set is built,
    // which a config-level projection cannot see and no encode row that
    // signals no siting would catch.
    uint32_t vuiChromaLocInfoPresent;
    uint32_t vuiChromaSampleLocTypeTop;
    uint32_t vuiChromaSampleLocTypeBottom;
    uint32_t rcUseMinQp;
    uint32_t rcUseMaxQp;
    int32_t  rcMinQpI;
    int32_t  rcMaxQpI;
    // H.265 only: EncoderConfigH265::GetCpbVclFactor()'s result, the quantity
    // ITU-T H.265 Table A.8 states. Projected because it is what a chroma-flag
    // / chroma_format_idc confusion silently gets wrong, and because every
    // downstream observable of it is ALSO a function of the level or tier, so
    // none of them reads the factor on its own.
    //
    // READ IN THE BINDER'S STATE, which is the state InitProfileLevel reads --
    // after InitializeParameters and before InitVideoProfile. The reading point
    // matters: the depth term reads encodeBitDepthLuma / encodeBitDepthChroma, and
    // deriving those in InitVideoProfile puts the derivation at session creation,
    // AFTER level selection -- so the level-selection call site would read zero
    // while InitRateControl read the real depth, from one function inside one
    // configuration. The derivation belongs in InitializeParameters, beside
    // encodeChromaSubsampling, so both call sites read the same value and this
    // projection reads it too. Zero on the other arms.
    uint32_t h265CpbVclFactor;
    // H.265 only: EncoderConfigH265::levelIdc as InitProfileLevel() selected
    // it, which is 30 x the level number. The factor's one DEVICE-FREE
    // downstream observable -- the default vbvBufferSize is not, because the
    // probe reads the config field and InitRateControl, which computes the
    // default from the factor, runs later and needs a session. A too-high
    // level is a legal level, which is why nothing caught the factor being
    // wrong; pinning it is what makes the correction visible downstream of the
    // arithmetic rather than only inside it. Zero on the other arms.
    uint32_t h265LevelIdc;
    // H.265 only: EncoderConfigH265::general_tier_flag, the OTHER half of what
    // DetermineLevelTier() picked. It is the term that actually moves with the
    // factor at 1080p: when main tier's bitrate ceiling (maxBitRateMainTier x
    // cpbVclFactor) is exceeded the selection does not climb to the next
    // level, it takes HIGH TIER at the same one -- so a level-only projection
    // reads the same number on a right and a wrong factor. Zero on the other
    // arms, which is also main tier, so this field is read together with
    // h265LevelIdc and not alone.
    uint32_t h265GeneralTierFlag;
    // AV1: the sequence-header colour config the arm attaches (AV1's
    // counterpart of the H.26x VUI colour description).
    uint32_t av1ColorDescriptionPresent;
    uint32_t av1ColorPrimaries;
    uint32_t av1TransferCharacteristics;
    uint32_t av1MatrixCoefficients;
    uint32_t av1ColorRange;
    uint32_t av1BitDepth;
    // Whether the arm attached a colour config AT ALL (pColorConfig !=
    // nullptr). Distinct from av1ColorDescriptionPresent, and the distinction
    // is the defect: AV1's color_config carries color_range, BitDepth and
    // subsampling as well as the colour description, so a caller that
    // declared only full range needs the STRUCT even though the description
    // flag stays 0. Gating the whole struct on that flag would drop the
    // range on AV1 alone, and no field below can tell "absent" from
    // "present and zero".
    uint32_t av1ColorConfigPresent;
    uint32_t av1ChromaSamplePosition;
    // The sequence header's STRUCTURAL subsampling, as the arm wrote it.
    // Projected because its correctness is decided by a DIFFERENT function
    // (InitProfileLevel, which picks seq_profile from the same input) than the
    // one that writes it, and no device this project can obtain reaches the arm
    // where the two disagree: AV1 High and Professional are absent from every
    // driver available here, so a 4:4:4 or 4:2:2 AV1 session dies at the
    // capability query before a sequence header exists. Device-free is the only
    // place this fact is assertable at all.
    uint32_t av1SubsamplingX;
    uint32_t av1SubsamplingY;
    // HDR10 static metadata, as the chained VkVideoEncoderHdrMetadataInfo
    // landed in EncoderConfig. It reaches the config through a pNext walk
    // rather than a config field, so nothing in the flat field table above
    // can see whether it bound.
    uint32_t hdrMasteringPresent;
    uint32_t hdrContentLightPresent;
    uint32_t hdrMaxDisplayMasteringLuminance;
    uint32_t hdrMinDisplayMasteringLuminance;
    uint32_t hdrMaxContentLightLevel;
    uint32_t hdrMaxFrameAverageLightLevel;
    // displayPrimaryX[0] / displayPrimaryY[0], i.e. ST 2086's GREEN. One
    // pair is enough to catch a permuted or dropped array and keeps the
    // projection from turning into a second copy of the struct.
    uint32_t hdrGreenPrimaryX;
    uint32_t hdrGreenPrimaryY;
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
//
// |requestedEncodeBitDepth| IS WHAT MAKES THE TWO GEOMETRIES SEPARABLE, and
// it exists for one reason. EncoderConfig::InitializeParameters derives the
// encode side from the input side under a zero-means-unset guard, and states
// beside that guard that "an explicit encode depth, if one is ever set before
// this runs, is a request and not a default". Until this parameter there was
// no way to set one, so the guard had a rationale and no mechanism -- and,
// more to the point, the encode and input sides were EQUAL ON EVERY REACHABLE
// STATE. A test written against equal values cannot say which of them a codec
// arm read: every assertion it makes is satisfied identically either way, so
// it is a guard against a wrong DERIVATION and no guard at all against a
// wrong SIDE. That is exactly the shape three arms regressed into once.
//
// Non-zero, it writes encodeBitDepthLuma before InitializeParameters runs, so
// the guard leaves it alone and the encode side differs from the input side
// on the depth axis. The profile a codec arm then derives says which side it
// read. Zero -- the default -- is the ordinary path and changes nothing, so
// every existing caller of the three-argument form is unaffected.
//
// IT IS NOT A BACK DOOR ONTO THE PUBLIC SURFACE. VkVideoEncoderConfig has no
// encode-depth field and this parameter reaches no public entry point; it is
// this header's, and this header is the internal one.
VkResult VkEncBuildAndProbeConfig(const VkVideoEncoderConfig& extConfig,
                                  VkVideoCodecOperationFlagBitsKHR codecOp,
                                  VkEncBoundConfigProbe* outProbe,
                                  uint32_t requestedEncodeBitDepth = 0);

// Byte-exact projection of the HDR10 payload builders.
//
// The two builders live below the ext layer (VkVideoEncoderHdrMetadata.h,
// which this header deliberately does not name -- the same rule that keeps
// EncoderConfig out of it), and the only production caller is inside a codec
// arm that needs a device and a driver-written parameter-set buffer. This
// wrapper takes the PUBLIC struct and hands back the bytes, so the payload
// can be pinned from a device-free test.
//
// That matters most for AV1, whose end-to-end path needs a device with AV1
// encode; the OBU bytes can be pinned without one.
//
// |codecOp| selects H.265 (a prefix SEI NAL, start code included) or AV1
// (metadata OBUs). Returns the byte count, or 0 if there was nothing to
// build or it did not fit.
uint32_t VkEncBuildHdrMetadataPayload(const VkVideoEncoderHdrMetadataInfo* info,
                                      VkVideoCodecOperationFlagBitsKHR codecOp,
                                      uint8_t* out, uint32_t capacity);

// ---------------------------------------------------------------------------
// Input taxonomy.
//
// Adapting a caller's input to what the device encodes has three rungs --
// hardware conversion, the compute filter, a transfer copy -- and the input's
// DECLARATION decides which of them are candidates:
//
//   ENCODABLE_DIRECT     the device takes this input as an encode source as
//                        it stands. Rung 1.
//   ENCODABLE_VIA_FILTER rung 2, and rung 2 ONLY -- a transfer copy is not a
//                        substitute. The 3-plane family differs from the
//                        semi-planar encode format by PLANE COUNT, and a copy
//                        cannot drop or merge a plane; the packed 4:4:4
//                        Y'CbCr layouts declared as such (AYUV, Y410) differ
//                        by plane count the other way, one interleaved plane
//                        against two; the 8-bit RGBA family
//                        (R8G8B8A8_UNORM, B8G8R8A8_UNORM,
//                        A8B8G8R8_UNORM_PACK32) differs by COLOUR MODEL, and
//                        a copy cannot convert colour at all. Both the RGBA
//                        family and the packed layouts are single-plane, so
//                        plane count does not follow from the class.
//   UNSUPPORTED          neither, in this build.
//
// THE DECLARATION, NOT THE FORMAT. A VkFormat names a component layout, and
// the packed 4:4:4 Y'CbCr layouts share their enumerants with RGBA, so the
// format alone cannot place them. VkVideoEncoderColorModel is what the caller
// states and what this function reads; VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT
// asks for the format's own answer and is what every caller that has no packed
// input passes. A declaration the format cannot carry is UNSUPPORTED, never
// silently reconciled.
//
// A property of the DECLARATION alone, so this is a free function and a test
// can drive every arm with no device. Whether a given SESSION can take an
// ENCODABLE_VIA_FILTER input is a second question -- the compute filter has
// to be compiled in, enabled, and able to read the specific image -- and it
// is answered by VulkanVideoEncoderExtImpl::SupportsFormat /
// ValidateImageDescriptor, which hold the session state this function
// deliberately does not.
enum VkEncInputFormatClass {
    VK_ENC_INPUT_FORMAT_UNSUPPORTED = 0,
    VK_ENC_INPUT_FORMAT_ENCODABLE_DIRECT,
    VK_ENC_INPUT_FORMAT_ENCODABLE_VIA_FILTER,
};
VkEncInputFormatClass VkEncClassifyInput(VkFormat inputFormat,
                                         VkVideoEncoderColorModel colorModel);

// "Could this library take this input on SOME path?" -- i.e. classified as
// anything but UNSUPPORTED.
//
// Not used inside the library: every gate here needs the finer answer, which
// is the class itself. It remains for an out-of-tree consumer that has the
// coarse question to ask.
VkBool32 VkEncSupportsInput(VkFormat inputFormat,
                            VkVideoEncoderColorModel colorModel);

// The number of planes |inputFormat| is laid out in, which EncoderConfig
// needs in order to reconstruct input.vkFormat at all: it does not store the
// input format, it DERIVES it from chroma subsampling, bit depth and
// numPlanes. 0 for a format this library does not classify.
//
// Takes no colour model, and that is a property of the question rather than
// an omission: plane count is a fact about the LAYOUT, and the packed 4:4:4
// layouts are one plane whichever model is declared over them.
uint32_t VkEncInputFormatPlaneCount(VkFormat inputFormat);

// Every input format this library routes, in a fixed order. The
// advertisement walks this list; the classifier answers what each one is.
//
// DERIVED, NOT LISTED. The set is computed from the multi-planar Y'CbCr
// format table and the packed 4:4:4 table -- the same two tables the compute
// filter's shader generator reads -- so the list and the classifier cannot
// disagree: both are the same predicate over the same rows. |outCount| is
// therefore the derived count and is not a constant of this header;
// VK_ENC_MAX_ROUTABLE_INPUT_FORMATS bounds it.
//
// Built once, on first call, and immutable afterwards.
const VkFormat* VkEncRoutableInputFormats(uint32_t& outCount);

// The encoder-input format |inputFormat| is converted INTO, given the
// formats the device accepts for the profile in question.
//
// For a Y'CbCr input the answer preserves the input's own chroma subsampling
// and bit depth and names the two-plane semi-planar form: the compute filter
// converts plane layout and packing, and resamples neither chroma nor depth,
// so a target that changed either would describe a conversion that does not
// happen.
//
// For an RGB input the answer is the device's FIRST advertised
// encode-source format, which is the same one the session takes: an RGB
// session states no encode-source request, precisely so that a packed 4:4:4
// alias cannot be matched by enum against a genuine RGBA input.
//
// VK_FORMAT_UNDEFINED for an input this library does not convert, and for an
// empty device list.
VkFormat VkEncConversionTargetFormat(VkFormat inputFormat,
                                     const VkFormat* deviceFormats,
                                     uint32_t deviceFormatCount);

// Decides whether ONE candidate input format is advertisable and, if so, what
// it is encoded as and by which route. Writes |outEntry| only when it returns
// true; |outEntry->format| is pre-set to the candidate, so an admission that
// only fills in encodeFormat and optimality is complete.
//
// THE PARAMETER IS THE WHOLE POINT. The production caller supplies the LIVE
// per-candidate resolver -- the same one the point query answers from, so the
// two surfaces cannot drift -- and the device-free tests supply a synthetic
// one built from a static device list. What is being tested through the
// synthetic one is everything BUT the admission rule: the ordering, the
// de-duplication, the capacity stop, and the build gate.
typedef bool (*VkEncInputFormatAdmitFn)(
    void* userData, VkFormat candidate,
    VkVideoEncoderInputFormatProperties* outEntry);

// The advertised input-format list for one profile: every format this library
// can route to an encoder input the device accepts, each naming what it is
// encoded as. Writes at most |outCapacity| entries and returns how many were
// written.
//
// EVERY CANDIDATE COMES FROM THE ROUTABLE LIST and is offered to |admit|
// exactly once. The admission decides membership and optimality; this function
// decides order, uniqueness and capacity.
//
// OPTIMAL entries come first, then SUBOPTIMAL ones, each group in the ROUTABLE
// list's order. It is not the device's order any more, and it cannot be: each
// candidate is now resolved at the profile its own binding derives, so there is
// no single device list to order by. Each format is written once however many
// tilings a device reports it at, because tiling is a property of an image and
// not of a format.
//
// NOT ADVERTISED AT ALL WHEN THE FILTER IS NOT COMPILED IN: every SUBOPTIMAL
// entry is ENCODABLE_VIA_FILTER and InitializeExt refuses exactly that class in
// such a build, so advertising one would name a format the library then
// refuses. The gate is the build's, not the admission's, and it is applied here
// so that no admission can bypass it.
//
// |outCapacity| is the caller's array length; the advertised list can never
// be longer than the routable list, so an array sized from that list holds
// every answer this function can give.
//
// A pure function of |admit|, so a test drives it with no device.
uint32_t VkEncAdvertiseInputFormats(
    VkEncInputFormatAdmitFn admit, void* userData,
    VkVideoEncoderInputFormatProperties* outEntries, uint32_t outCapacity);

// The buffer the library hands the DRIVER when it asks for a profile's
// VIDEO_ENCODE_SRC formats. A device list, not an advertised one.
enum { VK_ENC_MAX_DEVICE_INPUT_FORMATS = 16 };

// An upper bound on how many input formats this library can route, and so on
// how long an advertised list can be: every advertised entry names a distinct
// routable format.
//
// A BOUND, NOT A COUNT. The routable set is derived from the multi-planar
// Y'CbCr format table plus the three RGB spellings, so its size is a property
// of that table and moves when the table does. What this constant has to be
// is large enough to hold the derivation's answer, which the .cpp
// static_asserts against the table's own length rather than against a number
// written twice.
enum { VK_ENC_MAX_ROUTABLE_INPUT_FORMATS = 41 };

// Everything one (codec, profile) probe answers about a physical device: the
// scalar capabilities the public structure carries, and the std-syntax flag
// list, which is answered through its own two-call entry point.
//
// The list is held beside the scalars rather than inside them because a list in
// a public structure has to pick a capacity, and the capacity belongs to the
// caller. Inside the library the capacity is a private constant, sized from
// what the probe can actually produce.
//
// NO INPUT-FORMAT LIST. The probe issues one device format query per (codec,
// profile) at a fixed 4:2:0 envelope, which is the right envelope for the
// scalars and the wrong one for a format list -- a caller asking which formats
// it may feed the encoder is asking about the profile its own input derives,
// not about the one the probe happened to key on. The enumerator therefore
// resolves each candidate live, through the same function the point query
// answers from, and holds no list here to go stale against it.
enum { VK_ENC_MAX_STD_FLAG_ENTRIES = 4 };

struct VkEncProfileCapabilitySnapshot {
    VkVideoEncoderCapabilities caps;
    uint32_t                   stdFlagCount;
    VkVideoEncoderStdFlags     stdFlags[VK_ENC_MAX_STD_FLAG_ENTRIES];
};

// The colour model these samples are ACTUALLY in: the caller's declaration
// when one was made, and what the format says otherwise.
//
// The single point at which FROM_FORMAT is resolved, so no other site has to
// know which formats name their own model. Returns
// VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT -- never a guess -- for a format
// this library does not place at all, and for a declaration the format cannot
// carry; both are UNSUPPORTED to VkEncClassifyInput.
VkVideoEncoderColorModel VkEncResolveColorModel(
    VkFormat inputFormat, VkVideoEncoderColorModel declared);

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
// only place that can hand it to a caller. The carrier a caller chains is
// VkVideoEncoderImportGuardInfo, declared earlier in this header; this is the
// internal hop behind it, which has no sType and is not ABI.
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

// Which route a registered external image takes to the encoder.
//
//   DIRECT  the encoder reads the caller's image as it stands. The
//           registration's format, colour model, tiling and usage together
//           satisfy the direct predicate, and no copy and no conversion is
//           built.
//   STAGED  the image is copied into the library's own input pool. This is
//           the route for a registration that is not directly encodable and
//           needs no conversion either -- a pure tiling mismatch, say.
//   FILTER  the frame goes through the preprocess compute filter. Chosen
//           when the input classifies ENCODABLE_VIA_FILTER, this session
//           built the filter for that input, and the views the filter reads
//           were created on this image.
//
// An implementation choice and not a contract, which is why it is declared
// here: a caller negotiates what it may hand in, and the library decides
// what it does with what it is handed. Nothing on the public surface names
// this type or its values.
typedef enum VkVideoEncoderExternalInputPath {
    VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT = 0,
    VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED = 1,
    VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER = 2,
} VkVideoEncoderExternalInputPath;

// The registration-slot facts the release-obligation tests assert on. A
// dropped (or stranded) in-flight reference is observable only here: the
// public surface deliberately answers a stale id with RESOURCE_UNKNOWN
// whether the slot is retired, pinned, or gone.
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

// MID-STREAM CONSTANT-QP OBSERVATION SEAM.
//
// Folds any armed rate-control update and hands back the session
// CONSTANT-QP defaults that result -- the values EncodeFrameCommon copies
// into the next frame it processes.
//
// WHY THIS IS THE RIGHT OBSERVABLE. A DISABLED-mode session has no other
// session-level rate lever: the per-layer bitrates a rate-control command
// carries are dropped outright on such a session, because that mode
// commands layerCount 0. So the only way to tell a real constant-QP
// reconfigure from one that merely returned VK_SUCCESS is to read the
// value the next frame would be encoded with, which is what this reports.
//
// Null-backend sessions only (VK_ERROR_NOT_PERMITTED_KHR otherwise), and
// only once VkEncPushCapture has installed the device-free encoder behind
// the session. What this CANNOT assert is that the driver then honours the
// value -- that needs a GPU and a decoded comparison.
VkResult VkEncApplyAndGetSessionConstQp(VulkanVideoEncoderExt* encoder,
                                        int32_t* pQpIntra,
                                        int32_t* pQpInterP,
                                        int32_t* pQpInterB);

// MID-STREAM RATE-CONTROL OBSERVATION SEAM.
//
// Folds any armed update and reports what is IN FORCE afterwards, at the
// three depths a rate-control change has to survive:
//
//   * layer*      -- the live VkVideoEncodeRateControlLayerInfoKHR that
//                    HandleCtrlCmd copies verbatim into the next control
//                    command. This is where a maxBitrate of 0 shows up as
//                    the averageBitrate it was coerced to, and where a
//                    frameRateNum of 0 shows up as the frame rate that was
//                    left alone -- the values a caller-visible record of
//                    the configuration has to agree with.
//   * config*     -- the session config, where a QP clamp REQUEST lands.
//   * resolved*   -- the codec rate-control layer struct that request
//                    resolves to. This is the far end of the library-side
//                    chain and the struct CodecHandleRateControlCmd chains
//                    onto the command, so a configMinQp that moved while
//                    resolvedMinQpI did not is a clamp reaching nothing.
//
// codecRefreshCount counts re-invocations of the codec rate-control fill.
// It separates a real refresh from one that recomputed the same numbers,
// and it is what lets a test assert the NEGATIVE case: a bitrate-only
// update must not cause one.
//
// Null-backend sessions only (VK_ERROR_NOT_PERMITTED_KHR otherwise), and
// only once VkEncPushCapture has installed the device-free encoder behind
// the session. What this CANNOT assert is that the driver then honours any
// of it -- that needs a GPU and a decoded comparison.
typedef struct VkEncRateControlObservation {
    uint64_t layerAverageBitrate;
    uint64_t layerMaxBitrate;
    uint32_t layerFrameRateNumerator;
    uint32_t layerFrameRateDenominator;
    int32_t  constQpIntra;
    int32_t  constQpInterP;
    int32_t  constQpInterB;
    int32_t  configMinQp;
    int32_t  configMaxQp;
    uint32_t configMinQpSet;
    uint32_t configMaxQpSet;
    uint32_t resolvedUseMinQp;
    uint32_t resolvedUseMaxQp;
    int32_t  resolvedMinQpI;
    int32_t  resolvedMaxQpI;
    uint32_t codecRefreshCount;
} VkEncRateControlObservation;

VkResult VkEncApplyAndGetRateControl(VulkanVideoEncoderExt* encoder,
                                     VkEncRateControlObservation* pOut);

// THE RECORD Reconfigure COMPARES AGAINST, read back.
//
// Reconfigure keeps a copy of the configuration in force and refuses a
// later call that changes an immutable field, by comparing against this.
// The copy is also the session's own statement of what it is running,
// which is only worth anything if it agrees with the live rate-control
// state above -- and for a coerced maxBitrate or a dropped frame rate it
// did not. Reading both and comparing them is what makes that assertable
// rather than a matter of inspection.
//
// Null-backend sessions only.
VkResult VkEncGetRecordedConfig(VulkanVideoEncoderExt* encoder,
                                VkVideoEncoderConfig* pOut);

// Seed that record directly.
//
// A device-free session never runs InitializeExt, so its record is a
// default-constructed config: codec NONE, rate-control mode DEFAULT.
// Several of Reconfigure's refusals are keyed on what the session WAS
// initialized as -- a QP clamp change is refused on an AV1 session and on
// a constant-QP one -- and without this those branches could only be read,
// not run. Null-backend sessions only; pNext is cleared, as InitializeExt
// clears it.
VkResult VkEncSeedRecordedConfig(VulkanVideoEncoderExt* encoder,
                                 const VkVideoEncoderConfig* pConfig);

// Declare the device QP window the mid-stream clamp check reads.
//
// A real session records it from the codec capabilities in
// InitEncoderCodec. A device-free one has no capabilities to record, so
// the window would sit at "not established" and the check would be inert
// -- untestable rather than merely unexercised. Null-backend sessions
// only.
VkResult VkEncSetDeviceQpWindow(VulkanVideoEncoderExt* encoder,
                                int32_t minQp, int32_t maxQp);

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

// ---------------------------------------------------------------------------
// CAPABILITY-PROBE KEY OBSERVATION SEAM.
//
// The capability probe is keyed on (codec, profile, bit depth), because a
// Vulkan capability query is per VkVideoProfileInfoKHR and the depth is part
// of that structure. The public entry points name a profile by the codec
// standard number ALONE, which is enough for H.264 and H.265 -- there the
// number decides the depth -- and is NOT enough for AV1, whose seq_profile 0
// (Main) carries 8 or 10 bits. These read the probe tables directly so that
// the set of combinations the library can put to a driver is asserted on a
// runner with no encode-capable device, where no capability entry point can
// answer anything but "not present".
//
// They report what the library CAN ASK, never what a device answers. A device
// answer is measured on hardware or not at all.

// Is (codec, profile, bitDepth) a combination this library probes? bitDepth
// is in bits (8 or 10); any other value is false for every codec.
bool VkEncProbeNamesProfileBitDepth(VkVideoCodecOperationFlagBitsKHR codec,
                                    uint32_t profile,
                                    uint32_t bitDepth);

// How many probe rows the context snapshot carries for |codec|. Rows are what
// a context build issues one driver query each for; two rows may carry the
// same profile number at different depths.
uint32_t VkEncProbeSnapshotRowCount(VkVideoCodecOperationFlagBitsKHR codec);

// The (profile, bit depth) of snapshot row |slot| for |codec|. False when the
// codec has no such row. Either out pointer may be null.
bool VkEncProbeSnapshotRowAt(VkVideoCodecOperationFlagBitsKHR codec,
                             uint32_t slot,
                             uint32_t* outProfile,
                             uint32_t* outBitDepth);

#endif  // VULKAN_VIDEO_ENCODER_EXT_INTERNAL_H_
