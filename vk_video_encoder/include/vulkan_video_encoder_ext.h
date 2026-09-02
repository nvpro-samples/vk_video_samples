/*
 * Copyright 2024-2025 NVIDIA Corporation.
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

#ifndef _VULKAN_VIDEO_ENCODER_EXT_H_
#define _VULKAN_VIDEO_ENCODER_EXT_H_

#include "vulkan_video_encoder.h"
#include <vulkan/vulkan.h>

//=============================================================================
// API versioning and structure typing
//
// VK_VIDEO_ENCODER_EXT_API_VERSION identifies this release of the interface.
// It is NOT a compatibility counter: nothing raises it on a breaking change,
// and nothing reads it at runtime. Do not branch on it.
//
// Layout compatibility is enforced at BUILD TIME: a public struct that
// changes shape stops the build rather than passing silently.
//
// Skew across the boundary is closed by VENDORING. Take this header and the
// implementation together and build them as ONE unit; rolling either alone
// reintroduces the skew these rules exist to remove. Vendoring is also what
// covers the C++ interface below, where adding, removing or reordering a
// virtual method breaks the ABI exactly as a struct change does.
//
// Every public struct that rides a pNext chain opens with sType/pNext. Two
// structures carry no such prefix and chain nowhere: VkVideoEncoderPlaneLayout,
// an element of a fixed array inside another structure, and
// VkVideoEncoderInputFormatProperties, an element of an array the library
// writes. The rules below govern the chainable ones:
//   * The ONLY legal way to extend a struct is a new pNext-chained struct
//     with a new VkVideoEncoderStructureType value. Appending a field to an
//     existing struct is NOT an option: the sType is unchanged, so a consumer
//     built against the older layout still passes the gate and is then read
//     with a shifted layout -- silently. A version bump does not save it,
//     because nothing checks the version at runtime.
//   * sType values are never reused or renumbered.
//   * The library REJECTS a struct whose sType it does not recognize
//     rather than guessing -- VK_ERROR_INITIALIZATION_FAILED from the
//     entry points that return a VkResult,
//     VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN from those
//     that return a typed status.
//   * That rejection applies at EVERY public pNext position, entry and
//     result structs alike -- not only to each call's primary struct. An
//     unknown sType anywhere in a chain is a typed error, never a skip,
//     and a struct for which no extension struct is defined refuses ANY
//     chain. Where a call DOES define more than one optional link, the
//     walk is flat and order-independent in Vulkan's own manner: each
//     link is classified by its sType and not by its position, so a
//     known link is accepted wherever in that chain it sits, and a
//     repeated one is refused like an unknown one.
//   * There is deliberately NO size field: a size field invites reading
//     fewer bytes than the caller wrote -- silent field loss, the exact
//     defect class structure typing exists to prevent.
//
// Structs self-stamp via default member initializers, so `T x = {};` is
// correctly typed without a constructor call (IPC-deserialization safe).
//=============================================================================

// The release identifier for this interface, per the note above. There is no
// runtime version query and no version negotiation.
#define VK_VIDEO_ENCODER_EXT_API_VERSION 1

// Maximum planes in an imported image descriptor. Four covers every format
// this encoder accepts and keeps the descriptor a fixed-size POD.
// consecutiveBFrames: ask the driver for its preferred count instead of
// naming one. Distinct from 0, which means no B-frames.
#define VK_VIDEO_ENCODER_B_FRAMES_DRIVER_PREFERRED 0xFFFFFFFFu

// Maximum planes in an imported image descriptor. Four covers every format
// this encoder accepts.
#define VK_VIDEO_ENCODER_MAX_PLANES 4


typedef enum VkVideoEncoderStructureType {
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_UNDEFINED     = 0,
    // Private range base 'VE' << 16 (0x56450000); values are permanent.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG        = 0x56450001,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME   = 0x56450002,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT = 0x56450003,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_RUNTIME_INFO  = 0x56450004,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES  = 0x56450005,
    // Handle exchange. 0x5645000D .. 0x56450011 remain
    // reserved for the rest of the registry surface; never reuse.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR = 0x56450006,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_SYNC_DESCRIPTOR     = 0x56450007,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS              = 0x56450008,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT             = 0x56450009,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_SEMAPHORE_DESCRIPTOR      = 0x5645000A,
    // Chained onto VkVideoEncoderImageSupport::pNext: the renegotiation
    // modifier list (see the struct).
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT_DETAILS     = 0x5645000B,
    // The registration status echo.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS                    = 0x5645000C,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO = 0x56450012,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_DEADLINE_INFO = 0x56450013,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_VALIDATION_INFO = 0x56450014,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_DIAGNOSTIC_INFO = 0x56450015,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR = 0x56450016,
    // The context surface.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONTEXT_CREATE_INFO = 0x56450017,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_DEVICE_IDENTITY = 0x56450018,
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO = 0x56450019,
    // The input-residency side-channel (see VkVideoEncoderInputResidencyInfo).
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO = 0x5645001A,
    // The staged-input SUBMIT ENGINE side-channel; see
    // VkVideoEncoderStagedSubmitInfo. A new sType rather than two more fields
    // on VkVideoEncoderInputResidencyInfo, per the versioning rule the layout
    // pins in vulkan_video_encoder_ext.cpp enforce.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_STAGED_SUBMIT_INFO = 0x5645001B,
    // The dma-buf import-ordinal guard's verdict; see
    // VkVideoEncoderImportGuardInfo. Chainable in two places: onto
    // VkVideoEncoderStatus (the RegisterImageResource echo, where the
    // guard runs) and onto VkVideoEncoderCompletionInfo (the snapshot
    // call, readable at any time from any thread).
    //
    // Appended at the end of the range rather than taking one of the
    // 0x5645000D..0x56450011 registry reservations: those are promised to
    // registry structs not yet written, and spending one on a
    // driver-workaround report would quietly break that promise.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_GUARD_INFO = 0x5645001C,
    // The dma-buf import CONTENT verdict; see VkVideoEncoderImportContentInfo.
    // Chainable in the same two places the guard verdict is, and for the same
    // reason: a workaround whose failure is otherwise silent.
    //
    // It is a SEPARATE sType from the guard's rather than four more fields on
    // VkVideoEncoderImportGuardInfo, per the versioning rule the layout pins
    // in vulkan_video_encoder_ext.cpp enforce -- and also because the two
    // answer different questions. The guard reports what the
    // phase-shifting import-ordinal workaround did; this reports what the
    // driver actually put in the buffer. A caller may want either without
    // the other.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO = 0x5645001D,
    // HDR10 static metadata; see VkVideoEncoderHdrMetadataInfo. Chained onto
    // VkVideoEncoderConfig::pNext.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO = 0x5645001E,
    // How the caller's OWN samples are coded; see
    // VkVideoEncoderInputColourInfo. Chained onto VkVideoEncoderConfig::pNext.
    VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO = 0x5645001F,
} VkVideoEncoderStructureType;

//=============================================================================
// HDR10 STATIC METADATA -- SMPTE ST 2086 mastering display volume and
// MaxCLL/MaxFALL content light level.
//
// Chain onto VkVideoEncoderConfig::pNext. Absent, the encoder emits no SEI
// and no metadata OBU, which is what every caller wanted until one did not --
// the same reason VkVideoEncoderFrameDeadlineInfo is an extension.
//
// WHERE IT LANDS.
//   H.265  a PREFIX SEI NAL carrying mastering_display_colour_volume
//          (payloadType 137) and/or content_light_level_info (144), appended
//          to the VPS/SPS/PPS the driver writes -- so it repeats at EVERY
//          IDR, which is what a random-access point needs.
//   AV1    METADATA_TYPE_HDR_MDCV (2) and/or METADATA_TYPE_HDR_CLL (1)
//          metadata OBUs, appended to the sequence header OBU.
//   H.264  NOTHING, and that is a refusal rather than an omission: H.264 has
//          no standard mastering-display or content-light SEI. An H.264
//          session that chains this struct is REJECTED at InitializeExt
//          rather than accepting metadata it will not carry.
//
// UNITS ARE SMPTE ST 2086's, i.e. exactly what the H.265 SEI codes and what a
// producer already holds (Chromium's gfx::HdrMetadataSmpteSt2086, ffmpeg's
// AVMasteringDisplayMetadata): chromaticity in increments of 0.00002
// (coordinate x 50000), luminance in increments of 0.0001 cd/m^2 (nits x
// 10000). AV1 codes the same quantities in DIFFERENT fixed-point formats and
// in a DIFFERENT primary order; the library converts. The caller supplies one
// spelling and never sees the other.
//
// A DISPLAY OF ALL ZEROS IS A CLAIM, not an absence -- it says the mastering
// display is black -- so the two payloads have explicit presence flags and
// are emitted independently. A caller that knows MaxCLL and nothing about the
// mastering display sets contentLightLevelPresent alone.
//
// DO NOT LEAVE THIS CHAINED ON A CONFIG YOU PASS TO Reconfigure(). That entry
// point refuses ANY pNext -- see its structure-type gate -- because nothing
// it can change is extensible, and this metadata is one of the things it
// cannot change: it is written into the parameter sets at the first IDR
// alongside colourPrimaries / transferCharacteristics / matrixCoefficients /
// videoFullRange, all four of which Reconfigure already lists as immutable.
// A caller that builds one VkVideoEncoderConfig and hands it to both entry
// points must clear pNext for the Reconfigure call. Changing the colour
// volume mid-stream needs a session re-init.
struct VkVideoEncoderHdrMetadataInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_HDR_METADATA_INFO;
    const void*                 pNext = nullptr;

    // ---- mastering_display_colour_volume / metadata_hdr_mdcv ----
    VkBool32 masteringDisplayPresent = VK_FALSE;
    // ST 2086 order: index 0 GREEN, 1 BLUE, 2 RED. Coordinate x 50000.
    uint16_t displayPrimaryX[3] = {0, 0, 0};
    uint16_t displayPrimaryY[3] = {0, 0, 0};
    uint16_t whitePointX = 0;
    uint16_t whitePointY = 0;
    // cd/m^2 x 10000. E.g. 1000 nits is 10000000; 0.0001 nits is 1.
    uint32_t maxDisplayMasteringLuminance = 0;
    uint32_t minDisplayMasteringLuminance = 0;

    // ---- content_light_level_info / metadata_hdr_cll ----
    VkBool32 contentLightLevelPresent = VK_FALSE;
    uint16_t maxContentLightLevel      = 0;   // MaxCLL,  cd/m^2
    uint16_t maxFrameAverageLightLevel = 0;   // MaxFALL, cd/m^2
};

//=============================================================================
// THE INPUT'S OWN COLOUR CODING.

// The input's quantisation range. UNDECLARED IS NOT LIMITED: see the per-lane,
// per-codec policy table below, and note that on the DIRECT lane the library
// applies nothing, so this declaration is a FACT about the caller's samples
// and not a request.
typedef enum VkVideoEncoderRangeDeclaration {
    VK_VIDEO_ENCODER_RANGE_UNDECLARED = 0,
    VK_VIDEO_ENCODER_RANGE_LIMITED    = 1,   // studio swing / narrow
    VK_VIDEO_ENCODER_RANGE_FULL       = 2,
} VkVideoEncoderRangeDeclaration;

// How the caller's own samples are coded. Chain onto VkVideoEncoderConfig::pNext.
//
// THE PRINCIPLE: the caller declares what its buffer IS; the library declares
// what it DID. The colour fields on VkVideoEncoderConfig state what the
// BITSTREAM should advertise; these state what the INPUT carries. They are
// different questions and, apart from the transfer axis, they were one field.
//
// AN ABSENT CHAIN IS "UNDECLARED", ON EVERY AXIS AT ONCE, and produces exactly
// the previous behaviour bit for bit. There is no sentinel to discover and no
// value pattern to reverse-engineer from a binder gate. A caller that says
// nothing is no worse off than it was, and it becomes able to say so on
// purpose.
//
// 0 IS UNDECLARED ON EACH AXIS, matching VkVideoEncoderConfig's own colour
// fields, so a partly-filled chain is truthful field by field.
//
// WHAT THE LIBRARY DOES WITH IT. The RGBA->Y'CbCr preprocess filter derives its
// conversion matrix from inputColourPrimaries when this chain declares them.
// When it does not, the derivation falls back to
// VkVideoEncoderConfig::colourPrimaries -- an OUTPUT field -- and that fallback
// is sound ONLY because this library implements NO primaries conversion, so an
// input's primaries and its bitstream's primaries are necessarily the same. It
// is stated here rather than left implicit precisely because it stops being
// sound the moment a primaries conversion exists.
//
// WHEN BOTH ARE DECLARED AND THEY DISAGREE on an axis this library cannot
// convert, initialization is REFUSED with the reason -- the same rule
// VkVideoEncoderConfig::inputTransferCharacteristics already applies to the
// transfer axis. One mechanism, three more axes.
//
// THE RANGE AXIS HAS THE OTHER RULE, AND IT IS THE ONE WORTH READING. The
// three axes above are COMPARED on both lanes and applied on neither. The
// range is APPLIED on the Y'CbCr lane and COMPARED on the RGB lane, because
// that is where the library's own behaviour divides:
//
//   * Y'CbCr in. Nothing scales the samples on the way to the encoder, so the
//     range they carry IS the range the bitstream codes. A declared
//     inputRange therefore WRITES video_full_range_flag and raises
//     video_signal_type_present_flag with it -- see the policy table below
//     for why an unsignalled range is not a safe silence. It does not have to
//     agree with videoFullRange; it decides, and it is refused only against
//     videoFullRange == VK_TRUE, which is the one direction that is a real
//     contradiction (VK_FALSE is a VkBool32's zero and cannot be told from
//     silence).
//
//   * RGB in. The RGBA->Y'CbCr filter PRODUCES the output range, from
//     videoFullRange. The caller's declaration is about its RGB buffer, which
//     is a different quantity, so it does not retarget the output. The filter
//     samples its RGB over the full range and performs no input expansion, so
//     a declared LIMITED describes a conversion that does not happen and is
//     REFUSED with the reason, exactly as a mismatched matrix is; a declared
//     FULL agrees with what the filter reads and constrains nothing else.
//
// So a declared input range is recorded AND USED, and this sentence is what
// the taxonomy test's range case asserts rather than what it assumes.
//
// THE PER-LANE, PER-CODEC POLICY FOR "UNDECLARED". This is what no document
// said, and the AV1 row is the one that must not be folded into the others.
//
//   H.264 / H.265, DIRECT lane (Y'CbCr in, Y'CbCr out), primaries / transfer /
//     matrix undeclared: the library writes NOTHING and leaves
//     video_signal_type_present_flag at 0. Nothing was applied, so it has
//     nothing to say, and a decoder infers Unspecified on all three.
//
//   H.264 / H.265, DIRECT lane, RANGE undeclared: the library writes nothing --
//     AND BOTH HALVES OF WHAT THAT MEANS NEED SAYING. Normatively,
//     video_full_range_flag is nested inside video_signal_type_present_flag
//     and, absent, "shall be inferred to be equal to 0" (H.264 E.2.1, H.265
//     E.3.1) -- studio swing. OBSERVABLY, decoders do not apply that inference
//     at their API boundary: ffprobe reports color_range=unknown for an absent
//     description and tv for an explicit 0, and hands UNSPECIFIED downstream
//     for a scaler or a compositor to guess. A DECLARE-NOTHING CALLER IS
//     THEREFORE IMPLEMENTATION-DEPENDENT, NOT SAFELY LIMITED. A caller that
//     cares must declare.
//
//   H.264 / H.265, DIRECT lane, RANGE DECLARED: it is written, and it is
//     SIGNALLED. video_signal_type_present_flag goes up with video_format 5
//     (Unspecified) beside it, so a declaration survives as something a
//     decoder reads back rather than as something a caller has to hope was
//     inferred. This is the one axis on which a declaration about the INPUT
//     changes the bitstream, and it does so because on this lane the input's
//     range is the bitstream's range -- there is no conversion between them
//     to make the two questions different.
//
//   H.264 / H.265, FILTER lane (RGB in), matrix and range undeclared: BT.709
//     and limited are APPLIED and NOT SIGNALLED. The conversion did happen and
//     the library knows what it did; whether it should also say so is a
//     separate decision and is not taken here. videoFullRange is what moves
//     the applied range on this lane, and it moves the signalled one with it,
//     because they are one variable.
//
//   H.264 / H.265, FILTER lane, RANGE DECLARED: a declaration of FULL is
//     accepted and changes nothing -- it agrees with the full-range RGB the
//     filter reads -- and a declaration of LIMITED is REFUSED, because no
//     input expansion exists to honour it. What the STREAM carries stays
//     videoFullRange's to say on this lane.
//
//   H.264 / H.265, FILTER lane, primaries and transfer undeclared: Unspecified
//     (2). Nothing converted them, and 2 is the honest code point.
//
//   AV1, ANY lane, RANGE: THERE IS NO ABSENT STATE. color_range is
//     unconditional AV1 syntax (AV1 5.5.2 color_config()): a mandatory f(1) on
//     the mono-chrome and general paths, and assigned 1 on the sRGB/identity
//     path. So SOME value is in every AV1 bitstream whether this library writes
//     it or a driver does, and the library COMMITS TO ONE ON EVERY AV1 STREAM:
//     0 (studio) when nothing was declared. That is a per-codec asymmetry, not
//     a per-lane one, and it is stated here so no caller has to rediscover it.
//     A declaration is what makes the committed value the caller's rather than
//     the default; there is no "signalled or not" half of the question to ask
//     on this codec, only which bit.
//
//   AV1, primaries / transfer / matrix undeclared: color_description_present_flag
//     is 0 and the OBU omits all three. Symmetrical with H.26x.
//
// WHAT A CONSUMER DOES WITH THIS. A producer that knows its buffer -- a
// compositor handing over BT.2020 RGBA, a camera pipeline handing over full-
// range BT.601 Y'CbCr -- states it here and states the bitstream it wants on
// the config, and the library either honours the pair or refuses it. A producer
// that does not know says nothing and gets the behaviour it had. Neither has to
// model the library's routing to decide which fields to set, which is the
// property that lets a consumer delete its copy of the route taxonomy.
//
// DO NOT LEAVE THIS CHAINED ON A CONFIG YOU PASS TO Reconfigure(), which
// refuses ANY pNext. Every colour axis is already immutable for the life of the
// session, so this chain inherits the right lifetime; a caller that hands one
// VkVideoEncoderConfig to both entry points must clear pNext for the
// Reconfigure call.
struct VkVideoEncoderInputColourInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_COLOUR_INFO;
    const void*                 pNext = nullptr;

    // ISO/IEC 23091-2 / 23091-4 code points, as the config's output-side
    // fields use. 0 on any axis is UNDECLARED and asserts nothing.
    uint8_t  inputColourPrimaries         = 0;
    // Mirrors VkVideoEncoderConfig::inputTransferCharacteristics and is
    // subject to the identical rule: this library applies no transfer
    // function, so a non-zero value that differs from
    // transferCharacteristics is refused. Supplying both is allowed and they
    // must agree.
    uint8_t  inputTransferCharacteristics = 0;
    uint8_t  inputMatrixCoefficients      = 0;
    uint8_t  reserved                     = 0;   // must be 0
    // The quantisation range the caller's own samples carry. On a Y'CbCr
    // input this DECIDES the bitstream's range, because the library scales
    // nothing between them; on an RGB input it is checked against what the
    // filter can read and the bitstream's range stays
    // VkVideoEncoderConfig::videoFullRange's to state. See the range
    // paragraph above for both halves and for the one contradiction that is
    // refused.
    VkVideoEncoderRangeDeclaration inputRange = VK_VIDEO_ENCODER_RANGE_UNDECLARED;
};

// Optional extension of VkVideoEncoderConfig: chain this onto the config's
// pNext to override the per-frame completion deadline. Absent, the default
// (8 s) applies.
struct VkVideoEncoderFrameDeadlineInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_DEADLINE_INFO;
    const void*                 pNext = nullptr;

    // Nanoseconds; 0 selects the default (8 s). A non-zero value below 6 s is
    // raised to 6 s: any lower and a slow-but-successful frame would be
    // declared timed out and its real capture discarded as late, converting a
    // slow frame into a lost one.
    // A frame past its deadline is delivered by the Acquire methods as a
    // 0-byte drop frame with status VK_TIMEOUT (it still requires
    // ReleaseEncodedFrame); the session continues.
    uint64_t frameCompletionTimeoutNs = 0;
};

// Opt-in validations performed once, at InitializeExt (see
// VkVideoEncoderValidationInfo below).
typedef enum VkVideoEncoderValidationFlagBits {
    // Hard-fail InitializeExt (VK_ERROR_EXTENSION_NOT_PRESENT) when the
    // session's device lacks an extension the external-input import surface
    // uses, NAMING each missing one in the log, instead of passing creation
    // and failing later at import.
    //
    // SCOPE DIFFERS BY DEVICE OWNERSHIP. On the library-created device this
    // validates ENABLEMENT. On an IMPORTED device Vulkan offers no way to read
    // the enabled-extension set, so it validates PHYSICAL-DEVICE SUPPORT only
    // and enablement remains the device creator's to audit.
    VK_VIDEO_ENCODER_VALIDATE_EXTENSIONS_BIT = 0x00000001,
} VkVideoEncoderValidationFlagBits;
typedef uint32_t VkVideoEncoderValidationFlags;

// Optional extension of VkVideoEncoderConfig: chain onto the config's
// pNext. Unknown flag bits are rejected at InitializeExt
// (VK_ERROR_INITIALIZATION_FAILED): a caller asking for a validation this
// build does not know is not getting it, and must hear so.
struct VkVideoEncoderValidationInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_VALIDATION_INFO;
    const void*                 pNext = nullptr;

    VkVideoEncoderValidationFlags flags = 0;
};

//=============================================================================
// Explicit input-image residency (queue-family ownership).
//
// FOREIGN names an image owned by VK_QUEUE_FAMILY_FOREIGN_EXT -- a dma-buf or
// pixmap import -- which the encoder must acquire from that family before it
// may read the pixels. LOCAL names one allocated on the encoder's own device,
// where such an acquire is unnecessary and, on host-written content, illegal
// (VUID-VkImageMemoryBarrier2-srcStageMask-03854). Getting it wrong is not a
// performance nicety in either direction; see
// VkVideoEncoderExternalImageDescriptor::residency.
//
// THE DISCRIMINATOR IS THE ALLOCATOR, NOT THE WRITER. "Host-written" does
// not imply LOCAL: the Wayland zero-copy lane host-writes its pixels through
// a mapping and is correctly FOREIGN, because GBM/DRM allocated the buffer.
// Conversely an OS-handle import can be correctly LOCAL -- a self-import,
// where the image was allocated on the encoder's own device, exported and
// re-imported into it. Ask who allocated the memory and on whose queue family
// it sits, never who last wrote its contents.
//
// THE LAYOUT QUALIFIES THE DECLARATION. A frame presented in
// VK_IMAGE_LAYOUT_PREINITIALIZED is treated as local whatever residency it
// declared, because that layout names host-written content, which cannot be
// combined with a queue-family transfer.
//
// AUTO INFERS FROM THE LAYOUT: FOREIGN iff currentLayout !=
// VK_IMAGE_LAYOUT_PREINITIALIZED. That inference is correct only for
// FIRST-USE local staging images. A REUSED CPU-staging image is left in
// TRANSFER_SRC_OPTIMAL, which AUTO misclassifies as a foreign import. A
// CALLER POOLING OR REUSING INPUT IMAGES MUST STATE THE RESIDENCY
// EXPLICITLY, and that holds on every handle type. On an OS handle AUTO never
// reaches the layout inference at all: it is derived as FOREIGN.
//=============================================================================
enum VkVideoEncoderInputResidency {
    VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO    = 0, // inferred; see above
    VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL   = 1, // same-device allocation, no QFOT
    VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN = 2, // FOREIGN_EXT-owned import, QFOT acquire
};

//=============================================================================
// Handle exchange (design section 2)
//
// The library imports external memory itself. A consumer describes what it
// exported and hands over an OS handle; it does not create a VkImage, and it
// does not need to know what the encoder's device is.
//
// HANDLE OWNERSHIP -- one unconditional rule:
//
//   * The library CONSUMES a POSIX fd it is given. Always, on every exit
//     path, success or failure. After any call that takes an fd, the caller
//     must not close it, use it, or look at it; it is closed exactly once,
//     and not by the caller.
//
//   * The library NEVER closes a Win32 handle. Not on success, not on
//     failure. The caller retains it and must CloseHandle it once the
//     resource is unregistered.
//
// The asymmetry is Vulkan's: OPAQUE_FD and DMA_BUF transfer ownership to the
// implementation on import, NT handles do not.
//
// That rule is the TRANSFER mode -- the default, and what {} gives you. The
// mode is chosen per REGISTRATION, never per call outcome; see
// VkVideoEncoderHandleOwnership below. Both modes answer the same on every
// exit path, and the library REPORTS which rule it applied through
// VkVideoEncoderStatus::handlesConsumed (defined below), so a caller can
// assert rather than guess.
//=============================================================================

// Ownership mode, set per REGISTRATION -- not per frame, and never per
// outcome:
//
//   TRANSFER: the rule above, verbatim. After the call the handle is not
//   the caller's, on every exit path, success or failure. This is the
//   default, and what {} gives you.
//
//   BORROW: the handle stays the caller's, on every exit path, success or
//   failure. The library duplicates it before anything that can fail and
//   applies the TRANSFER rule to its own copy. The caller closes its
//   original whenever it likes.
//
// BORROW exists for a caller that must import one received handle twice -- a
// single frame fd into both a display device and the encoder, say. It is
// meaningless across an IPC boundary, where the transport already duplicated
// the handle. On Win32 both modes are identical (the library never closes an
// NT handle) and BORROW is accepted as a no-op.
typedef enum VkVideoEncoderHandleOwnership {
    VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_TRANSFER = 0,
    VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_BORROW   = 1,
} VkVideoEncoderHandleOwnership;

typedef enum VkVideoEncoderExternalHandleType {
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_NONE            = 0,
    // POSIX: ownership transfers to the library (see above).
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_FD       = 1,
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF         = 2,
    // Win32: caller retains.
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_OPAQUE_WIN32    = 3,
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_D3D11_TEXTURE   = 4,
    // An already-imported image on the ENCODER's own device. The
    // same-device path; no import is performed and no handle is consumed.
    VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE        = 5,
} VkVideoEncoderExternalHandleType;

// Typed status: a refusal a caller can act on. A bare VkResult cannot tell
// "this modifier is not encodable" from "wrong GPU" from "that format is
// unsupported", and a client has to know whether to renegotiate the
// allocation or fail the stream.
typedef enum VkVideoEncoderStatusCode {
    VK_VIDEO_ENCODER_STATUS_SUCCESS = 0,
    VK_VIDEO_ENCODER_STATUS_ERROR_STRUCTURE_TYPE_UNKNOWN,
    // Reserved; nothing returns it. Not removed, because the values are
    // positional and deleting one renumbers every code below it.
    VK_VIDEO_ENCODER_STATUS_ERROR_API_VERSION_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_DEVICE_MISMATCH,
    VK_VIDEO_ENCODER_STATUS_ERROR_HANDLE_TYPE_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_EXTENSION_MISSING,
    VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_USAGE_INSUFFICIENT,
    VK_VIDEO_ENCODER_STATUS_ERROR_PLANE_LAYOUT_INVALID,
    VK_VIDEO_ENCODER_STATUS_ERROR_ALLOCATION_SIZE_INVALID,
    VK_VIDEO_ENCODER_STATUS_ERROR_MEMORY_TYPE_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_CONVERSION_REQUIRED,
    VK_VIDEO_ENCODER_STATUS_ERROR_IMPORT_FAILED,
    VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT,
    VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN,
    VK_VIDEO_ENCODER_STATUS_ERROR_NOT_INITIALIZED,
    // New codes are APPENDED here, never inserted: every value above and
    // below is ABI for a consumer built against an older header.
    VK_VIDEO_ENCODER_STATUS_ERROR_SHARING_MODE_UNSUPPORTED,
    VK_VIDEO_ENCODER_STATUS_ERROR_EXTENT_INVALID,
    // Flow control, not an error -- VkResult's VK_NOT_READY as a status:
    // the submit path is at capacity, so drain completions and retry the
    // same call. Distinct from ERROR_RESOURCE_LIMIT above, which retrying
    // alone never clears.
    //
    // ERROR_RESOURCE_LIMIT has TWO sources. (1) A registry's 4096-slot image
    // or semaphore table is full; an Unregister clears it. (2) A DIRECT
    // submit was handed more waits than its eight-entry array can carry;
    // presenting fewer waits clears that, and unregistering does nothing for
    // it. Read the code as "this cannot succeed until the request or the
    // registry changes" -- it does not by itself say which, and an unchanged
    // retry succeeds in neither case.
    VK_VIDEO_ENCODER_STATUS_NOT_READY,
    // The colorModel DECLARATION cannot be read against the format it was
    // declared over: RGB named over a Y'CbCr format, Y'CbCr named over an
    // RGBA layout that carries no packed 4:4:4 reading, or a value the
    // enumeration does not define.
    //
    // The offending field is colorModel, not format. NV12 declared RGB is
    // refused under this code while NV12 itself is directly encodable, so a
    // caller acting on the code alone corrects the declaration -- which is
    // the only thing that can be corrected. Changing the format instead
    // would answer a question that was never asked.
    //
    // Appended rather than placed among the other refusals: the values are
    // positional, and inserting one renumbers every code below it for every
    // consumer already built against this header.
    VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED,
} VkVideoEncoderStatusCode;

// Library-minted resource id, safe to carry across an IPC boundary. A stale
// id -- one minted before an unregister -- is REJECTED rather than silently
// matching a recycled slot.
typedef uint64_t VkVideoEncoderResource;
#define VK_VIDEO_ENCODER_RESOURCE_NULL ((VkVideoEncoderResource)0)

typedef struct VkVideoEncoderPlaneLayout {
    uint64_t offset;
    // NOT READ. The library writes 0 into the Vulkan structure itself, which
    // is what VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-size-02267
    // requires on the explicit path. The other four members of this structure
    // are taken as declared.
    uint64_t size;
    uint64_t rowPitch;
    uint64_t arrayPitch;   // 0 when arrayLayers == 1
    uint64_t depthPitch;   // 0 for 2D
} VkVideoEncoderPlaneLayout;

// The colour model a caller's samples are in.
//
// A Vulkan format names a COMPONENT LAYOUT, and for one family of inputs the
// layout does not determine the colour model. The packed 4:4:4 Y'CbCr layouts
// have no Vulkan enumerant of their own and ride the matching RGBA ones --
// AYUV on VK_FORMAT_R8G8B8A8_UNORM, Y410 on VK_FORMAT_A2B10G10R10_UNORM_PACK32
// -- which are also the enumerants an ordinary RGBA producer declares. The
// format alone therefore cannot say whether a texel holds red, green and blue
// or luma and two chroma, and the difference decides whether a colour
// conversion runs.
//
// Declaring the model is how a caller settles that. It is the caller's to
// state because it is a fact about the caller's own pixels, and nothing the
// library can measure recovers it.
typedef enum VkVideoEncoderColorModel {
    // Read the colour model off the format. Every Vulkan format except the
    // packed 4:4:4 Y'CbCr layouts above names exactly one, so this is the
    // right answer for all of them -- and it is what a zero-initialised
    // structure says, so a caller that has no packed input never sets this
    // field.
    VK_VIDEO_ENCODER_COLOR_MODEL_FROM_FORMAT = 0,
    // Red, green and blue, with the transfer function already applied. The
    // library converts to the encoder's Y'CbCr input using the matrix
    // VkVideoEncoderConfig::matrixCoefficients names.
    VK_VIDEO_ENCODER_COLOR_MODEL_RGB = 1,
    // Luma and two chroma. Over a packed 4:4:4 layout this is the
    // declaration that makes AYUV and Y410 nameable; over any other Y'CbCr
    // format it states what the format already says and changes nothing.
    VK_VIDEO_ENCODER_COLOR_MODEL_YCBCR = 2,
} VkVideoEncoderColorModel;

// Pointer-free POD: the struct IS the IPC payload, and the OS handle rides
// out of band (SCM_RIGHTS on POSIX, a duplicated handle on Windows). It is a
// superset of what a producer already holds about an image it exported, so
// a caller fills it by field copy.
typedef struct VkVideoEncoderExternalImageDescriptor {
    VkVideoEncoderStructureType sType;
    const void*                 pNext;  // MUST be NULL; a chain is refused

    VkVideoEncoderExternalHandleType handleType;

    // ---- The exporter's ACTUAL VkImageCreateInfo. Never guessed. ----
    VkFormat              format;
    uint32_t              width;
    uint32_t              height;
    VkImageType           imageType;    // 0 => VK_IMAGE_TYPE_2D
    uint32_t              mipLevels;    // 0 => 1
    uint32_t              arrayLayers;  // 0 => 1
    VkSampleCountFlagBits samples;      // 0 => VK_SAMPLE_COUNT_1_BIT
    VkImageTiling         tiling;       // OPTIMAL | LINEAR | DRM_FORMAT_MODIFIER
    // 0 is INVALID on the OS-handle arms, not a default: the library never
    // invents a usage the exporter did not grant. The VK_IMAGE arm alone may
    // leave it 0, and is then accepted as transfer-source only -- see
    // RegisterImageResource.
    VkImageUsageFlags     imageUsage;
    VkImageCreateFlags    imageFlags;
    VkSharingMode         sharingMode;
    // NOT mirrored: the exporter's create-time VkVideoProfileListInfoKHR. A
    // profile-DEPENDENT exporter therefore cannot be registered faithfully.
    //
    // WHICH MAKES VIDEO-PROFILE COMPATIBILITY A CALLER OBLIGATION ON THE
    // DIRECTLY ENCODABLE ARM, and one this library cannot discharge for it.
    // A registration whose tiling is not LINEAR, whose format the library
    // encodes without conversion, and which grants
    // VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR is encoded FROM THE CALLER'S
    // OWN IMAGE, with no staging copy in between. It must therefore ALREADY
    // be compatible with the video profile the session negotiates
    // (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-08206) -- created either with a
    // VkVideoProfileListInfoKHR naming that profile in its VkImageCreateInfo
    // pNext chain, or with VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR.
    // Registration is handed an image that already exists, so nothing done
    // here can repair one created with neither.
    //
    // PROFILE INDEPENDENCE IS NOT A BLANKET EXEMPTION. A profile-independent
    // image is compatible with a profile only while EVERY usage it declares
    // is one vkGetPhysicalDeviceVideoFormatPropertiesKHR reports for that
    // profile and format. VK_IMAGE_USAGE_STORAGE_BIT is commonly absent from
    // that set for encode-source formats, so an image carrying STORAGE may be
    // registered for the compute filter -- which reads it through a storage
    // descriptor, and for which STORAGE is required -- but must not ALSO
    // declare VIDEO_ENCODE_SRC and ask to be encoded directly. A producer
    // that wants both should present the image without VIDEO_ENCODE_SRC and
    // let the registration route through the staging copy.

    // ---- DRM format modifier ----
    // Separate presence flag because modifier 0 is REAL -- it is
    // DRM_FORMAT_MOD_LINEAR. A single uint64 cannot distinguish "no modifier,
    // import as OPTIMAL" from "the modifier is literally zero".
    VkBool32 hasDrmFormatModifier;
    uint64_t drmFormatModifier;

    uint32_t                  planeCount;
    // All planes must live in the SINGLE memory object named by the handle
    // passed to RegisterImageResource. A disjoint (multi-buffer-object) image
    // cannot be expressed here.
    //
    // THIS IS A CALLER OBLIGATION, and it cannot be delegated. Registration
    // is handed ONE handle, so the library cannot see where the other planes
    // came from; nothing in this descriptor carries their provenance.
    // RegisterImageResource rejects the shape that is provably impossible in
    // one allocation -- two planes at the same offset, which is what a
    // disjoint producer writes when every plane starts at the base of its own
    // buffer -- but a disjoint producer whose chroma plane sits at a non-zero
    // offset inside its OWN buffer passes that check and yields silently
    // wrong chroma. Do not treat the offset rule as a disjointness test.
    //
    // A caller that holds one handle per plane (gfx::NativePixmapHandle,
    // gbm_bo_get_fd_for_plane, ...) MUST compare their identity itself --
    // fstat st_dev/st_ino on POSIX -- and only then present the buffer here.
    // On NVIDIA/gbm the per-plane exports of one BO return the same cached
    // dma_buf, so equal (st_dev, st_ino) is positive proof of jointness, and
    // distinct BOs never collide. A caller that cannot run that test should
    // stage a copy instead of registering.
    VkVideoEncoderPlaneLayout planeLayouts[VK_VIDEO_ENCODER_MAX_PLANES];

    // ---- Memory, as reported by the EXPORTER ----
    // Deriving allocationSize from vkGetImageMemoryRequirements is wrong for
    // dma-buf on NVIDIA. 0 means unknown and is accepted with a log.
    uint64_t allocationSize;
    uint32_t memoryTypeBits;    // 0 = unknown (a real mask always has a
                                // bit set: the exporter's own allocation
                                // occupies a type)
    uint32_t memoryTypeIndex;   // UINT32_MAX = unknown
    // memoryTypeIndex is a PREFERENCE on the dma-buf arm and a REQUIREMENT
    // on the opaque arms:
    //  - DMA_BUF: the library asks vkGetMemoryFdPropertiesKHR which types
    //    can import THIS fd and honours memoryTypeIndex only inside that
    //    mask intersected with the image's requirements
    //    (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00648). If the query
    //    is unavailable, the legacy preference order runs as a logged
    //    fallback, never silently.
    //  - OPAQUE_FD / OPAQUE_WIN32: querying is forbidden and the import
    //    must reuse the exporter's allocation parameters
    //    (VUID-VkMemoryAllocateInfo-allocationSize-01742 / -01743), so
    //    supply memoryTypeIndex; on OPAQUE_FD, memoryTypeBits additionally
    //    lets the library choose safely when the exact index is not
    //    available. An OPAQUE_WIN32 import without it is refused: on WDDM
    //    a guessed type is a hard error, not a fallback.

    // ---- Provenance: multi-GPU safety ----
    // A supplied deviceUUID or deviceLUID that does not match the encode
    // device fails DEVICE_MISMATCH at registration, rather than failing the
    // import later with an unhelpful driver error. driverUUID binds on the
    // OPAQUE handle types, where the external-memory compatibility table
    // requires driver identity; a dma-buf is a kernel object and a
    // cross-driver import of one is legal, so a driverUUID difference there
    // is reported rather than refused.
    uint8_t  deviceUUID[VK_UUID_SIZE];
    uint8_t  driverUUID[VK_UUID_SIZE];
    uint8_t  deviceLUID[VK_LUID_SIZE];
    VkBool32 deviceLUIDValid;

    // THE LAYOUT THE LIBRARY WILL FIND THIS IMAGE IN ON THE FIRST FRAME.
    //
    // A STATEMENT OF FACT about the image, not a preference: it is named as
    // the oldLayout of the FIRST acquire barrier, so a false declaration
    // violates VUID-VkImageMemoryBarrier2-oldLayout-01197.
    //
    // ONE VALUE IS NOT NAMED BACK. VK_IMAGE_LAYOUT_UNDEFINED declared here
    // is read as "the producer stated nothing", and the library substitutes
    // a layout of its own rather than name UNDEFINED in a barrier -- an
    // UNDEFINED oldLayout permits the implementation to discard the very
    // pixels the producer just wrote. WHICH layout it substitutes is not part
    // of this contract: it depends on the path the registration takes, and a
    // directly encodable image does not get the same answer as one routed
    // through the staging copy. A caller that needs a known layout must
    // declare the layout the image is really in rather than rely on the
    // substitution. Every other layout is carried through unchanged as the
    // acquire oldLayout; for the layout the image is LEFT in between frames,
    // see below.
    //
    // WHEN THE LIBRARY STOPS ASKING. On the STAGED path it moved the image
    // itself, so from the second frame on it KNOWS the layout and names its
    // own record rather than this
    // field. There, a declaration that was true once does not have to be kept
    // true across frames the caller never touched the image on. That record is
    // not written on every path, and where it is absent this field is re-read
    // every frame -- the enumeration is below.
    //
    // WHERE THE LIBRARY LEAVES THE IMAGE BETWEEN FRAMES is the library's
    // choice and is not part of this contract. It differs by path -- a staged
    // image and a directly encodable one are not handed back the same way --
    // and it is constrained by what a barrier may name as a destination
    // (VUID-VkImageMemoryBarrier2-newLayout-01198), which excludes
    // VK_IMAGE_LAYOUT_UNDEFINED and VK_IMAGE_LAYOUT_PREINITIALIZED. A caller
    // that needs the image in a particular layout between frames must put it
    // there itself and state it per frame, on
    // VkVideoEncoderFrameSubmitInfo::currentLayout, rather than infer it from
    // what was declared here.
    //
    // TO SEE WHAT THE LIBRARY ACTUALLY NAMED, set VKENC_DEBUG_LAYOUT in the
    // environment and the library traces the layouts it puts in its
    // barriers. DIAGNOSTIC ONLY, and deliberately so: it is the only way to
    // observe the two answers this contract has just declined to specify --
    // which layout is substituted for UNDEFINED, and where the image is
    // left between frames -- and observing them does not turn either into a
    // promise. Do not branch on what it reports. It writes to stderr
    // directly, so unlike the library's other diagnostics it is NOT
    // suppressed by silenceStdio.
    //
    // WHEN THIS FIELD STOPS BEING READ. On the STAGED path the library moved
    // the image itself, so from the second frame on it names its own record
    // instead: a declaration that was true once does not have to be kept true
    // across frames the caller never touched the image on. TO OVERRIDE THAT
    // RECORD ON A GIVEN FRAME -- because you moved the image yourself between
    // submits -- set VkVideoEncoderFrameSubmitInfo::currentLayout for that
    // frame. A non-UNDEFINED value there is an explicit per-frame statement
    // of fact and beats the record; UNDEFINED there resolves to the record
    // where one was kept, and to this field where it was not.
    //
    // THREE CASES WHERE THIS FIELD IS RE-READ EVERY FRAME, because no record
    // exists or it is not authoritative:
    //  * a DIRECTLY ENCODABLE registration, which is never staged;
    //  * an image the library RELEASED to VK_QUEUE_FAMILY_FOREIGN_EXT, since a
    //    foreign agent held it between frames and may have transitioned it,
    //    and the release CLEARS the record rather than leave a stale one;
    //  * the SubmitExternalFrame lane, which has no registration at all and
    //    takes its layout from the frame.
    //
    // KNOWN GAP, FIRST FRAME. The library does not transition the image at
    // registration, so this declaration must ALREADY be true when the first
    // frame is submitted, and only the caller can make it so. An image with
    // external memory can be created only UNDEFINED
    // (VUID-VkImageCreateInfo-pNext-01443), so on the OS-handle tiers the
    // image the library imports genuinely IS in UNDEFINED on frame 1,
    // whatever is declared here -- and declaring UNDEFINED does not carry
    // that fact through, because UNDEFINED is the one value not named back.
    // No declaration therefore makes frame 1 truthful on those tiers; the
    // caller's remaining lever is to transition the image into the declared
    // layout itself before the first submit.
    VkImageLayout defaultLayout;


    // HONOURED ON EVERY HANDLE TYPE. An explicit LOCAL or FOREIGN is a
    // statement of fact by the only party that can make it, and the library
    // takes it as given wherever it is made -- with one layout-driven
    // qualification, and only on the STAGED path. There, a frame presented in
    // VK_IMAGE_LAYOUT_PREINITIALIZED is treated as local even under a FOREIGN
    // declaration, because PREINITIALIZED names host-written content, which
    // is not a legal pairing with a queue-family ownership transfer
    // (VUID-VkImageMemoryBarrier2-srcStageMask-03854). The DIRECT path reads
    // this field alone, with no layout term. An explicit LOCAL carries no
    // qualification on either path.
    //
    // IT IS NOT DERIVED FROM THE HANDLE TYPE. That the library performed the
    // import does not make the memory foreign to the encode device: a
    // SELF-IMPORT -- an image allocated on THIS device, exported, and
    // re-imported into it, which is what an embedder does when it wants the
    // library to own the staging allocation -- has no second device and no
    // second queue family. The discriminator is OWNERSHIP BY AN EXTERNAL
    // ALLOCATOR OR QUEUE FAMILY, never who wrote the pixels; see the enum
    // above for the two cases that make the difference.
    //
    // AUTO (== 0, so also what a zero-initialised descriptor says) IS STILL
    // DERIVED AS FOREIGN ON AN OS HANDLE, and only there: a caller that
    // propagates no residency across a process boundary has told the library
    // nothing, and a genuine import is overwhelmingly the likelier reading.
    // On VK_IMAGE, AUTO continues to mean the layout-derived inference.
    //
    // GETTING IT WRONG IS NOT A PERFORMANCE NICETY, in either direction.
    // Declaring FOREIGN for a local image takes a two-sided queue-family
    // ownership transfer it does not need: an acquire FROM
    // VK_QUEUE_FAMILY_FOREIGN_EXT and, at the last use of the image, a
    // matching release BACK to it. The release is the expensive half to get
    // wrong -- it gives a local image away to an owner that does not exist
    // and discards the library's residual-layout record, so the next frame
    // acquires from a layout the image is no longer in. Declaring LOCAL for
    // a genuinely foreign one skips an acquire the transfer needs, and the
    // copy then reads undefined content.
    VkVideoEncoderInputResidency residency;

    // Only for handleType == VK_IMAGE: the caller's already-imported image.
    // Ignored for every OS-handle type, where the library performs the import
    // and therefore knows the answer without being told.
    VkImage       existingImage;

    // Ownership mode for the handle passed to RegisterImageResource (see
    // VkVideoEncoderHandleOwnership). Zero-init == TRANSFER, deliberately:
    // the safe thing is what {} gives you.
    VkVideoEncoderHandleOwnership ownership;

    // The colour model the samples in this image are in (see
    // VkVideoEncoderColorModel). Zero-init reads it off |format|, which is
    // correct for every format but the packed 4:4:4 Y'CbCr layouts.
    //
    // A declaration the format cannot carry is REFUSED at registration, and
    // at QueryImageSupport, which answers from the same predicate:
    // VK_VIDEO_ENCODER_STATUS_ERROR_COLOR_MODEL_UNSUPPORTED, which names
    // this field and not |format|. It is not reconciled to one of the two
    // readings. Nothing here can know which the caller meant, and choosing
    // produces a picture that is plausible and wrong everywhere; refusing at
    // the negotiation point is what lets the producer correct the
    // declaration while the allocation can still change.
    VkVideoEncoderColorModel colorModel;
} VkVideoEncoderExternalImageDescriptor;

// Per-frame submission against a registration.
//
// Everything describing the IMAGE lives in the registration; this carries
// only what genuinely varies per frame.
typedef struct VkVideoEncoderFrameSubmitInfo {
    VkVideoEncoderStructureType sType;  // ..._FRAME_PARAMS
    const void*                 pNext;

    VkVideoEncoderResource resource;  // from RegisterImageResource

    uint64_t frameId;
    uint64_t pts;
    VkBool32 forceIDR;
    VkBool32 isLastFrame;
    // -1 = no override. Honoured only in DISABLED (caller-managed) rate
    // control; in CBR/VBR the encoder owns QP.
    int32_t  qpOverride;
    // The layout the producer left the image in. VK_IMAGE_LAYOUT_UNDEFINED
    // means "as declared at registration".
    VkImageLayout currentLayout;

    uint32_t            waitSemaphoreCount;
    const VkSemaphore*  pWaitSemaphores;
    const uint64_t*     pWaitSemaphoreValues;
    uint32_t            signalSemaphoreCount;
    const VkSemaphore*  pSignalSemaphores;
    const uint64_t*     pSignalSemaphoreValues;
} VkVideoEncoderFrameSubmitInfo;

// Picture type of an encoded frame (VkVideoEncodeResult::pictureType).
typedef enum VkVideoEncoderPictureType {
    VK_VIDEO_ENCODER_PICTURE_TYPE_I = 0,  // intra (incl. IDR / intra-refresh)
    VK_VIDEO_ENCODER_PICTURE_TYPE_P = 1,
    VK_VIDEO_ENCODER_PICTURE_TYPE_B = 2,
} VkVideoEncoderPictureType;

// State of a submitted frame (GetFrameStatus). The query CANNOT fail.
// "Never submitted" and "already released" share UNKNOWN deliberately: both
// are caller bugs, and both call for the same response.
typedef enum VkVideoEncoderFrameState {
    VK_VIDEO_ENCODER_FRAME_STATE_UNKNOWN  = 0,  // never submitted, or already released
    VK_VIDEO_ENCODER_FRAME_STATE_PENDING  = 1,  // submitted, no capture yet
    VK_VIDEO_ENCODER_FRAME_STATE_READY    = 2,  // an Acquire call will deliver it
    VK_VIDEO_ENCODER_FRAME_STATE_ACQUIRED = 3,  // delivered, awaiting ReleaseEncodedFrame
} VkVideoEncoderFrameState;

// Completion callback: invoked the moment a frame's capture becomes
// retrievable (the ONE producer-raised readiness edge). Contract:
//   * MUST NOT THROW. Unwinding out of the callback leaves the encoder in a
//     state with no route back; where the toolchain makes noexcept part of
//     the function type (C++17 and later) the compiler enforces this,
//     elsewhere the library catches and aborts, naming the frame.
//   * MUST NOT BLOCK -- post to your own executor and return (a consumer
//     that does work here stalls the encode pipeline).
//   * MUST NOT call session-serial (class a) methods; the library rejects
//     such re-entry with VK_ERROR_NOT_PERMITTED_KHR. The thread-safe
//     (class c) retrieval methods ARE legal here.
//   * MUST NOT destroy the encoder from inside the callback. Detach first
//     (SetCompletionCallback(nullptr, nullptr), which is itself a quiesce
//     point), then destroy; teardown joins the thread you are running on.
//   * Notifications COALESCE: one callback may cover several ready frames.
//     Drain in a loop and reconcile against GetCompletionCounter() --
//     assuming one-callback-one-frame silently drops frames under load.
//   * Deadline-synthesized VK_TIMEOUT drops do NOT raise the callback
//     (there is no capture); they become retrievable by deadline passage.
#if defined(__cplusplus) && (__cplusplus >= 201703L)
#define VK_VIDEO_ENCODER_CB_NOEXCEPT noexcept
#else
#define VK_VIDEO_ENCODER_CB_NOEXCEPT
#endif

typedef void (*PFN_vkVideoEncoderCompletionCallback)(uint64_t frameId,
                                                     void* pUserData)
    VK_VIDEO_ENCODER_CB_NOEXCEPT;

// Invoked EXACTLY ONCE when the encoder stops using a pUserData previously
// given to SetCompletionCallback -- on replacement, on detach, or at
// destruction -- and only after any in-flight completion invocation has
// returned.
//
// Prefer this to keeping the cookie alive by arrangement with the encoder's
// teardown order. A weak reference INSIDE the cookie protects the work the
// callback does; it does nothing for the cookie itself, which the encoder
// must dereference in order to reach it.
//
// Optional: pass nullptr to keep owning the cookie yourself.
typedef void (*PFN_vkVideoEncoderUserDataRelease)(void* pUserData)
    VK_VIDEO_ENCODER_CB_NOEXCEPT;

// Completion observability snapshot (GetCompletionInfo).
//
// Chainable onto pNext: VkVideoEncoderDiagnosticInfo,
// VkVideoEncoderFilterInfo, VkVideoEncoderInputResidencyInfo,
// VkVideoEncoderStagedSubmitInfo, VkVideoEncoderImportGuardInfo and
// VkVideoEncoderImportContentInfo. Unknown or repeated sTypes are refused,
// not ignored.
struct VkVideoEncoderCompletionInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    const void*                 pNext = nullptr;

    uint64_t completionCounter;  // monotonic frame-completion count (drain target)
    uint64_t framesTimedOut;     // deadline drops delivered
    uint64_t lateCaptures;       // captures discarded after a timeout drop
    uint64_t framesCancelled;    // CancelFrame/CancelAllPendingFrames
    uint32_t framesPending;      // submitted, no capture yet
    uint32_t framesReady;        // retrievable now
    uint32_t framesAcquired;     // delivered, awaiting ReleaseEncodedFrame
};

// Diagnostic side-channel: chain to VkVideoEncoderCompletionInfo::pNext
// on a GetCompletionInfo() call.
//
// The library's runtime misuse messages otherwise go to stderr, which
// silenceStdio discards -- so under silenceStdio a real API misuse (for
// example ReleaseEncodedFrame on an unknown frame id: a double release, or a
// typo'd id) is a SILENT no-op. Chaining this struct returns the running
// misuse count and the most recent message through the snapshot call the
// consumer already makes, so the text can land in the consumer's OWN logging.
//
// lastDiagnostic is NUL-terminated and empty until the first recorded
// misuse; the count never resets for the life of the encoder object.
#define VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS 192
struct VkVideoEncoderDiagnosticInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_DIAGNOSTIC_INFO;
    const void*                 pNext = nullptr;

    uint64_t diagnosticCount;  // misuses recorded since creation
    char     lastDiagnostic[VK_VIDEO_ENCODER_MAX_DIAGNOSTIC_CHARS];
};

// Which preprocess conversion the library BUILT for this session.
//
// A standalone enum rather than VulkanFilterYuvCompute::FilterType, for the
// same reason the profile constants are spelled out above: the public header
// carries no dependency on an internal one.
enum VkVideoEncoderFilterType {
    VK_VIDEO_ENCODER_FILTER_TYPE_NONE = 0,
    // Any YCbCr -> YCbCr conversion, including 3-plane I420 -> 2-plane NV12
    // (plane-count and bit-depth conversion) and the identity copy.
    VK_VIDEO_ENCODER_FILTER_TYPE_YCBCR_COPY = 1,
    VK_VIDEO_ENCODER_FILTER_TYPE_RGBA_TO_YCBCR = 2,
    VK_VIDEO_ENCODER_FILTER_TYPE_YCBCR_TO_RGBA = 3,
};

// Filter-dispatch side-channel: chain to VkVideoEncoderCompletionInfo::pNext
// on a GetCompletionInfo() call.
//
// WHAT THIS REPORTS. Whether a preprocess conversion ran, and how much of the
// session's work took it. The session's input format says whether the library
// BUILT a filter; it does not say what happened to any given frame, and the
// two genuinely diverge:
//
//   * VkVideoEncoder::StageInputFrame decides per FRAME, not per session
//     (`useComputeFilter` is recomputed for every frame from that frame's
//     own routing), so a configured session can send some or all frames
//     down the staging copy instead.
//   * The library's own stderr narration is not an alternative: a session
//     that sets silenceStdio discards every line the gated wrappers carry,
//     and this narration is among them. A "0 dispatches"
//     reading taken from stderr is
//     indistinguishable from a silenced one. This channel is structured
//     data through a call the consumer already makes, so it is immune.
//
// HOW TO READ IT. filterDispatchCount and stagedCopyCount are the two sides
// of one `if` in StageInputFrame and never both count the same frame, so
// they are compared directly rather than by subtracting from a total:
//
//   filterCreated == VK_FALSE                  -> no filter on this session
//   created, dispatch == 0, copy > 0           -> configured but DEAD; every
//                                                 frame took the copy arm
//   created, dispatch > 0, copy == 0           -> the filter is the path
//   dispatch == 0 && copy == 0                 -> nothing reached staging at
//                                                 all (e.g. a zero-copy arm
//                                                 that bypasses this stage),
//                                                 OR you read it too late --
//                                                 see below
//
// READ IT BEFORE Flush(), AND BEFORE THE LAST REFERENCE TO THE ENCODER GOES.
// All four fields are filled inside one `if (m_encoder)`, and Flush() and
// teardown clear m_encoder. A read after either therefore reports
// filterCreated = VK_FALSE with both counts 0 -- which is a legal, honest
// "no snapshot available" and is INDISTINGUISHABLE from "this session has no
// filter and never staged a frame". The bottom row of the table above is
// the one this trap lands on, which is why it is the row that names it.
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

//=============================================================================
// Input-residency side-channel: chain to VkVideoEncoderCompletionInfo::pNext
// on a GetCompletionInfo() call.
//
// THE PROBLEM THIS EXISTS FOR, and it is not the same problem
// VkVideoEncoderFilterInfo solves. Which of the two staging-acquire programs
// a registered external frame took -- the VK_QUEUE_FAMILY_FOREIGN_EXT
// ownership acquire, or the local HOST|TRANSFER availability barrier -- is
// decided inside the library from |residency| and the declared layout, and
// NOTHING crossed the ext boundary to say which one ran.
//
// That gap is not cosmetic, and it is why this channel was added rather than
// the question being answered from the validation layer. THE TWO BARRIER
// PROGRAMS ARE BOTH SELF-CONSISTENT AND BOTH LEAVE THE IMAGE IN THE SAME
// LAYOUT, so a run that takes the wrong one is VALIDATION-CLEAN: the acquire
// names FOREIGN as its source family and no release ever matched it, the
// queue-family transfer transfers nothing, and the caller's host writes lose
// their availability operation (srcStageMask NONE instead of HOST_BIT) -- and
// not one of those is a core VUID. A counter is therefore the only
// observable that can tell the two apart from outside.
//
// HOW TO READ IT. The two counts are the two sides of one `if` in
// VkVideoEncoder::StageInputFrame and never both count the same frame, so
// they are compared directly rather than by subtracting from a total:
//
//   foreign > 0, local == 0    -> every staged frame took the FOREIGN acquire
//   foreign == 0, local > 0    -> every staged frame took the local restore
//   both 0                     -> nothing reached the staging tier at all
//                                 (a DIRECT/zero-copy registration bypasses
//                                 it), OR you read it too late -- see below
//
// COUNTED AT THE ROUTING DECISION, NOT AT THE BARRIER. The increment sits
// after the last refusal return in StageInputFrame and before the copy/filter
// split, so it counts frames whose staging BARRIER PROGRAM WAS CHOSEN AND
// RECORDED, on either arm, exactly once. Counting inside the two arms' own
// release/handback pairs instead would need four sites and would undercount
// the moment a session mixed filtered and copied frames. A superset counter
// can make a live tier read as dead.
//
// EXTERNAL INPUT ONLY. The library's own file-input lane declares no
// residency and is deliberately not counted here: it would be
// indistinguishable from a caller's local registration and would make
// `local == frames` true on a session that registered nothing.
//
// READ IT BEFORE Flush(), AND BEFORE THE LAST REFERENCE TO THE ENCODER GOES,
// for exactly the reason VkVideoEncoderFilterInfo states above: both fields
// are filled inside one `if (m_encoder)`, and Flush() and teardown clear
// m_encoder, so a later read reports an honest but indistinguishable pair of
// zeros. DrainPendingFrames() does NOT clear it and is the right place to
// read after -- it joins the encoder threads, so every submitted frame has
// been through StageInputFrame by the time it returns.
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

//=============================================================================
// Staged-input SUBMIT ENGINE side-channel: chain to
// VkVideoEncoderCompletionInfo::pNext on a GetCompletionInfo() call.
//
// THE QUESTION THIS ANSWERS, and why neither existing channel answers it.
// VkVideoEncoderFilterInfo says whether a frame CONVERTED and whether it
// COPIED. VkVideoEncoderInputResidencyInfo says which of the two staging
// ACQUIRE PROGRAMS ran. Neither says WHICH QUEUE FAMILY any of that was
// recorded and submitted on -- and that is the fact a change to the session's
// DECLARED INPUT FORMAT moves, silently, for frames whose own format did not
// change at all.
//
// The mechanism, because it is not visible from outside the library.
// VkVideoEncoder::GetStagedInputSubmitType returns COMPUTE whenever a
// preprocess filter OBJECT exists on the session. That is a SESSION property,
// not a per-frame one. Both arms of StageInputFrame take their command buffer
// from m_inputCommandBufferPool, which InitEncoder creates on ONE family --
// the compute family when the filter exists, because the filter IS that pool --
// and a command buffer may only be submitted to a queue of its pool's family
// (VUID-vkQueueSubmit2-commandBuffer-03874). So a frame that dispatches NO
// filter and takes the plain-copy arm still has its FOREIGN -> local acquire,
// its ReleaseImageToForeignQueue and its submit on the COMPUTE family the
// instant the session gains a filter.
//
// WHY THAT IS A LIVENESS QUESTION AND NOT A PERFORMANCE NOTE. A driver may
// lose the device executing a queue-family RELEASE to
// VK_QUEUE_FAMILY_FOREIGN_EXT of an image created with
// VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR or ..._DPB_BIT_KHR when the
// barrier is recorded on a family other than graphics or optical flow. The
// usage bit is the gate there, not the family alone. The staged copy arm
// releases the CALLER'S IMPORTED image, whose usage is whatever the caller
// declared -- so whether a session-format widening is safe depends jointly
// on this family and on that usage, and both have to be READ rather than
// reasoned about.
//
// A validation layer cannot substitute for this: both submit families are
// validation-clean, so the engine is only observable by being reported.
//
// SESSION-CONSTANT. Filled from the same two accessors StageInputFrame and
// SubmitStagedInputFrame read, never re-derived in the reporting path, because
// a channel that re-derives the value cannot witness the one class of bug
// those two accessors exist to prevent -- the barrier site and the submit site
// naming different families. Valid even when no frame has staged yet.
//
// Read it BEFORE Flush() and before the last encoder reference goes, for the
// reason the two structs above give: the fields are filled under
// `if (m_encoder)`, and a torn-down session honestly reports
// submitTypeQueueFlags 0 / VK_QUEUE_FAMILY_IGNORED.
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

//=============================================================================
// External Frame Input with Synchronization
//
// Extends VulkanVideoEncoder for frame-at-a-time operation with
// externally-provided VkImages and timeline semaphore synchronization.
// This is the interface for cross-process encoder services.
//
// Usage flow:
//   1. CreateVulkanVideoEncoderExt() to create the encoder
//   2. InitializeExt() with structured config (not argc/argv)
//   3. For each frame:
//      a. RegisterImageResource() once per buffer, then
//         SubmitRegisteredFrame() naming that registration per frame.
//         SubmitExternalFrame() with a VkImage still works; registration is
//         what a cross-process or pooled producer needs.
//      b. Wait for the completion callback (SetCompletionCallback), then
//         drain with AcquireNextEncodedFrame in a LOOP -- notifications
//         coalesce. Polling still works but is not the intended shape.
//   4. Flush() to drain pending frames
//=============================================================================

//=============================================================================
// Encode profile
//
// A profile is named by THE CODEC STANDARD'S OWN NUMBER: H.264 profile_idc
// (ITU-T H.264 Annex A, Table A-1), H.265 general_profile_idc (ITU-T H.265
// Annex A), AV1 seq_profile (AV1 6.4.1). VkVideoEncoderConfig::profile is a
// plain uint32_t read against VkVideoEncoderConfig::codec, so the value space
// belongs to the standard and not to this header: a profile the constants
// below do not name is still expressible, and whether this library and this
// device can encode it is answered by
// EnumerateVulkanVideoEncoderProfileCapabilities* and, failing that, by a
// refusal at InitializeExt naming the value. A profile request is never
// accepted and ignored.
//
// The constants below carry the same values as StdVideoH264ProfileIdc /
// StdVideoH265ProfileIdc / StdVideoAV1Profile, spelled out so this header
// puts no <vk_video/...> dependency onto a consumer. THEY ENUMERATE EVERY
// PROFILE THIS LIBRARY BINDS, and that is a checked property rather than a
// promise: the taxonomy test sweeps the whole value space of each codec's
// profile syntax element through the binder and fails when a number binds
// that no constant here names, and when a constant here names a number the
// binder refuses. A library change that widens the bind set therefore fails
// until this block is widened with it.
//
// NAMING THEM IS WHAT KEEPS THE HEADER SELF-SUFFICIENT. Without a name a
// caller that wants High 4:4:4 Predictive -- which is what the DEFAULT
// derivation itself selects from 4:4:4 input -- must either write the bare
// integer 244 or include <vk_video/vulkan_video_codec_h264std.h>, which is
// exactly the dependency this block exists to spare it.
//
// THE VALUES ARE THE STANDARD'S, NOT A LIBRARY NUMBERING, so a profile added
// to a standard is expressible here on the day it is assigned, and the only
// thing a library change adds is the ability to BIND it.
//
// VK_VIDEO_ENCODER_PROFILE_DEFAULT (0) asks the library to derive the profile
// from the input's bit depth and chroma subsampling. It is 0 so that a
// zeroed config selects the derivation on every codec instead of a real
// profile.
//
// ONE OVERLAP, STATED RATHER THAN DISCOVERED. AV1 seq_profile 0 IS Main, and
// is therefore the same value as DEFAULT. On an AV1 session 0 is read as
// "derive", and the derivation reads both halves of the input: 8/10-bit
// 4:2:0 picks Main, which is what seq_profile 0 names, so the two readings
// agree there; 4:4:4 picks High and 12-bit or 4:2:2 picks Professional,
// none of which seq_profile 0 can carry. 0 is not a legal H.264 profile_idc
// and is not an assigned H.265 general_profile_idc, so no other codec
// carries the overlap.
#define VK_VIDEO_ENCODER_PROFILE_DEFAULT 0u

// H.264 profile_idc. WHAT EACH ONE ADMITS IS THE STANDARD'S RULE, on both
// axes, and a request against input the profile cannot carry is refused at
// InitializeExt rather than emitted out-of-spec. Per ITU-T H.264 Annex A,
// Table A-1: Baseline, Main and High are 8-bit 4:2:0; High 10 reaches ten
// bits at 4:2:0; High 4:2:2 reaches ten bits and 4:2:2; High 4:4:4
// Predictive reaches fourteen bits and 4:4:4.
enum VkVideoEncoderProfileH264 {
    VK_VIDEO_ENCODER_PROFILE_H264_BASELINE            = 66,
    VK_VIDEO_ENCODER_PROFILE_H264_MAIN                = 77,
    VK_VIDEO_ENCODER_PROFILE_H264_HIGH                = 100,
    VK_VIDEO_ENCODER_PROFILE_H264_HIGH_10             = 110,
    VK_VIDEO_ENCODER_PROFILE_H264_HIGH_422            = 122,
    VK_VIDEO_ENCODER_PROFILE_H264_HIGH_444_PREDICTIVE = 244,
};

// H.265 general_profile_idc. Main is 8-bit 4:2:0 (H.265 A.3.2), Main Still
// Picture is Main's single-picture form (A.3.4) and admits the same input,
// Main 10 reaches ten bits at 4:2:0 (A.3.3), and Range Extensions and Screen
// Content Coding Extensions reach 4:2:2, 4:4:4 and sixteen bits (A.3.5,
// A.3.7). Input a named profile cannot carry is refused; DEFAULT derives a
// profile that admits the input on both axes.
enum VkVideoEncoderProfileH265 {
    VK_VIDEO_ENCODER_PROFILE_H265_MAIN                    = 1,
    VK_VIDEO_ENCODER_PROFILE_H265_MAIN10                  = 2,
    VK_VIDEO_ENCODER_PROFILE_H265_MAIN_STILL_PICTURE      = 3,
    VK_VIDEO_ENCODER_PROFILE_H265_FORMAT_RANGE_EXTENSIONS = 4,
    VK_VIDEO_ENCODER_PROFILE_H265_SCC_EXTENSIONS          = 9,
};

// AV1 seq_profile. Main is 8/10-bit 4:2:0, High is 8/10-bit 4:4:4, and
// Professional is the one that reaches 4:2:2 and twelve bits (AV1 6.4.1,
// A.2). Note the overlap above: Main's value is also
// VK_VIDEO_ENCODER_PROFILE_DEFAULT, so it is the one profile a caller cannot
// name distinctly from the derivation -- which costs nothing, because on
// 4:2:0 input at 8 or 10 bits the derivation selects it.
enum VkVideoEncoderProfileAV1 {
    VK_VIDEO_ENCODER_PROFILE_AV1_MAIN         = 0,
    VK_VIDEO_ENCODER_PROFILE_AV1_HIGH         = 1,
    VK_VIDEO_ENCODER_PROFILE_AV1_PROFESSIONAL = 2,
};

// The capability enumeration answers per (codec, profile) pair, so a profile
// this library binds but this device cannot encode is reported there rather
// than discovered at session creation.
//=============================================================================

//=============================================================================
// Encoder Configuration (structured, not argc/argv)
//=============================================================================
struct VkVideoEncoderConfig {
    VkVideoEncoderStructureType sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    const void*                 pNext = nullptr;

    // Codec
    VkVideoCodecOperationFlagBitsKHR codec;

    // Encode profile within the selected codec: the codec standard's own
    // profile number, read against |codec| above. The profile constants above
    // name every number this library binds -- checked, not asserted -- and
    // state what input each one admits.
    // VK_VIDEO_ENCODER_PROFILE_DEFAULT (0) derives the profile from the input.
    uint32_t profile = VK_VIDEO_ENCODER_PROFILE_DEFAULT;

    // Encode output resolution
    uint32_t encodeWidth;
    uint32_t encodeHeight;

    // Input format -- what the external frames will be. Together with
    // |inputColorModel| below it is the whole of what the library's decision
    // about the preprocess conversion is made from.
    //
    // THE LIBRARY DECIDES WHETHER A CONVERSION RUNS. The caller does not ask
    // for one, and there is no flag to set.
    //
    // WHETHER A FORMAT IS ACCEPTED is decided by the declared pair -- this
    // field and |inputColorModel| -- TOGETHER WITH the codec, the profile and
    // the device, and InitializeExt settles both halves before it creates a
    // session.
    //
    //   * The LIBRARY half is settled with no device involved: a pair this
    //     library does not route, or one the named profile cannot carry, is
    //     refused with the reason and is not renegotiable by trying another
    //     GPU.
    //   * The DEVICE half is settled against the profile the configuration
    //     derives: a pair this device will not encode is refused NAMING the
    //     format, its chroma subsampling and its bit depth, rather than
    //     surviving to the driver's own refusal at video-session creation --
    //     which arrives later and names neither.
    //
    // VkEncEnumerateInputFormats IS THAT ANSWER, ASKED BEFORE THE CALL. It
    // lists the formats this library can route to an encoder input that a
    // given device accepts for a given (codec, profile), each flagged OPTIMAL
    // or SUBOPTIMAL -- and it is computed by the same function InitializeExt
    // gates on. So for a (codec, profile) the advertised set IS the accepted
    // set: a format on the list initialises, and a format the list omits is
    // refused. There is one surface to consult, not two.
    //
    // THE ONE AXIS THE LIST CANNOT CARRY IS THE COLOUR MODEL, and it is why
    // VkEncQueryInputFormatSupport takes one and the list does not. The packed
    // 4:4:4 Y'CbCr layouts AYUV and Y410 have no Vulkan enumerant of their own
    // and ride the RGBA ones, so they are accepted only where
    // |inputColorModel| declares them; undeclared, the same enumerant is an
    // ordinary RGBA image. A list keyed on formats alone can show neither of
    // them as itself -- AYUV's enumerant appears there under its RGB reading,
    // and Y410's enumerant does not appear at all -- so on THAT axis, and only
    // that one, absence from the list is not a refusal. The point query is
    // where a caller holding AYUV or Y410 frames gets the answer, and it gives
    // the same verdict this call will.
    //
    // TWO CONSEQUENCES OF THE SAME RULE, spelled out because they decide what
    // a caller allocates:
    //
    //   * Y416 (VK_FORMAT_R16G16B16A16_UNORM) is not accepted under any
    //     declaration: 16 bits per component is not an encode component bit
    //     depth. It is on no list and is refused under every colour model,
    //     so the two agree about it as they do about every other format.
    //   * On a Y'CbCr input the chroma subsampling OF THE FORMAT is what the
    //     encode profile is derived from, so a 4:4:4 Y'CbCr input encodes as
    //     4:4:4 without anything further being asked for. An RGBA input
    //     carries no subsampling to read and selects no profile this way:
    //     what its bitstream is coded at is the encodeFormat of its
    //     advertised entry, per VkVideoEncoderInputFormatProperties.
    //
    // A format this library does not accept -- under either reading, where
    // the enumerant carries two -- is refused at InitializeExt with the
    // reason.
    //
    // A conversion this build or this device cannot perform is an init
    // failure with a reason, never a quiet acceptance. The
    // two ways the conversion can be unavailable are the filter not being
    // compiled into the build (CMake BUILD_ENCODER_COMPUTE_FILTER) and the
    // session's device exposing no compute queue family to run it on.
    //
    // HOW A CALLER FINDS OUT, BEFORE IT ALLOCATES A FRAME POOL:
    // SupportsFormat() answers for one format on this session, and
    // QueryImageSupport() answers for a whole image descriptor and returns
    // CONVERSION_REQUIRED naming what is missing. Those queries are the
    // surface the decision is visible through.
    VkFormat inputFormat;

    // ALPHA IS NOT ENCODED, AND THAT IS A REFUSAL RATHER THAN AN OMISSION.
    // The RGBA spellings above are accepted as inputs and their A channel is
    // DROPPED: the preprocess filter reads R, G and B, and what the encoder
    // is handed is the Y'CbCr the matrix produced. There is nowhere for the
    // alpha to go. Vulkan Video's encode extensions define no auxiliary
    // picture layer, no alpha component and no second coded picture, and
    // VkVideoProfileInfoKHR carries exactly three colour dimensions -- chroma
    // subsampling, luma depth, chroma depth -- with no fourth. So this is an
    // API-level absence, not a device one, and no driver changes it.
    //
    // THE OUT-OF-BAND ROUTE, for a caller that needs transparency: encode the
    // colour here as usual and encode the alpha plane as the LUMA of a SECOND
    // stream from a second encoder instance, with neutral chroma, then carry
    // the two elementary streams together in the container. Several instances
    // per process are supported, so the composition belongs above this
    // library. This header names the route so that "the encoder does not do
    // alpha" is not mistaken for "alpha cannot be carried".

    // The colour model |inputFormat|'s samples are in (see
    // VkVideoEncoderColorModel). Zero-init reads it off |inputFormat|.
    //
    // The session is declared in this PAIR: |inputFormat| alone does not name
    // an input, because the packed 4:4:4 Y'CbCr layouts share their enumerants
    // with RGBA. Both halves are properties of the frames the session will be
    // given, so both are stated once here and neither is sent per frame.
    //
    // IMMUTABLE ACROSS Reconfigure(), as |inputFormat| is. That call compares
    // what the declaration RESOLVES to: naming the model the format already
    // carries, or leaving the field FROM_FORMAT, resolves to what is in force
    // and is accepted, while a declaration that resolves to the other model
    // -- which is what the packed 4:4:4 readings of the RGBA enumerants are
    // -- is refused rather than accepted and ignored.
    VkVideoEncoderColorModel inputColorModel;

    uint32_t inputWidth;
    uint32_t inputHeight;

    // Rate-control mode: the Vulkan enum directly. NOTE the values are bit
    // flags -- DEFAULT = 0, DISABLED_BIT = 1, CBR_BIT = 2, VBR_BIT = 4
    // (VBR is 4, not 3).
    VkVideoEncodeRateControlModeFlagBitsKHR rateControlMode =
        VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    uint32_t averageBitrate;    // bits/sec
    uint32_t maxBitrate;        // bits/sec (VBR)
    uint32_t vbvBufferSize;     // bits (0 = default)

    // Constant quantizer for I, P and B pictures, applied when
    // rateControlMode is DISABLED. -1 means "this config does not name that
    // quantizer" and leaves it to the library, which resolves P from I and B
    // from P.
    //
    // UNITS ARE THE CODEC'S OWN, AND THE RANGE FOLLOWS THE UNIT:
    //
    //     H.264 / H.265   a QP,               0 .. 51
    //     AV1             a quantizer index,  0 .. 255
    //
    // A value ABOVE the codec's maximum is REFUSED, with
    // VK_ERROR_INITIALIZATION_FAILED and a message on the error stream
    // naming the field, the value and the range. It is not clamped, not
    // reinterpreted and not truncated. Both entry points enforce this:
    // InitializeExt() and Reconfigure(). 52 is therefore a legal AV1
    // quantizer index and a refused H.26x QP -- one range does not serve
    // both codecs, and a caller that computes these values has to know which
    // codec it is configuring.
    //
    // 0 is legal on every codec and is carried as given.
    int32_t constQpI;
    int32_t constQpP;
    int32_t constQpB;
    // Rate-control QP clamps for CBR/VBR sessions, H.26x units (QP 0..51).
    // 0 = unset: no clamp reaches the driver. An explicit minQp of 0
    // collapses onto unset BY DESIGN -- QP 0 is the codec floor, so
    // "clamp at 0" and "no lower clamp" admit the same QP range. A maxQp
    // of 0 (force every frame to QP 0) is NOT expressible through these
    // fields; expressing it would take a pNext-chained struct with a new
    // sType, per the ABI rules at the top of this header -- not a wider field
    // here, and not a version bump, which nothing reads. AV1 rate
    // control is quantizer-index based (0..255, a different unit): a
    // non-zero value on an AV1 session is REJECTED at InitializeExt
    // rather than reinterpreted or ignored. Values are validated against
    // the codec range at InitializeExt and against the device's supported
    // QP window at device init.
    int32_t minQp;
    int32_t maxQp;

    // GOP structure. gopLength 0 leaves the GOP length unstated, and the
    // device's preferred GOP length applies; see idrPeriod below for the
    // matching rule on IDRs.
    uint32_t gopLength;         // Frames per GOP
    // B-frames between I/P pictures.
    //
    //   0                                       -> no B-frames (IPPP).
    //   1 .. 254                                -> that many B-frames.
    //   VK_VIDEO_ENCODER_B_FRAMES_DRIVER_PREFERRED -> let the driver choose.
    //
    // Driver-preferred has its own value rather than overloading 0, so that
    // a caller asking for no reordering cannot silently get mini-GOPs.
    // Anything else above 254 is rejected at init; 255 is reserved.
    uint32_t consecutiveBFrames;
    // Frames between IDRs. 0 does NOT mean "no IDRs" and does not mean "IDR
    // on every frame": it leaves the period UNSTATED, and the device's own
    // preferred IDR period -- reported per quality level and adopted when
    // the session reads its device capabilities -- is what applies. It is
    // read independently of gopLength. On a device that reports no
    // preference the period stays unstated and only the first frame of the
    // stream is an IDR, so a caller that needs random access at a known
    // cadence states the period here.
    uint32_t idrPeriod;
    VkBool32 closedGop;

    // Frame rate
    uint32_t frameRateNum;
    uint32_t frameRateDen;

    // (The per-frame completion deadline lives in
    // VkVideoEncoderFrameDeadlineInfo, chained onto this struct's pNext --
    // see the ABI rules at the top of this header.)

    // Quality
    uint32_t qualityLevel;      // 0 = default

    // Tuning mode. LOSSLESS engages transquant-bypass + QP0 in the codec
    // config, producing bit-exact output (per the Vulkan spec); it requires
    // rateControlMode DISABLED.
    VkVideoEncodeTuningModeKHR tuningMode =
        VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR;

    // Colour info (VUI). ISO/IEC 23091-4 code points, shared verbatim by
    // H.264, H.265 and AV1.
    //
    // 0 MEANS "NOT SUPPLIED" on this surface, and each of the three is read
    // INDEPENDENTLY. Supplying only transferCharacteristics = 16 (PQ)
    // declares PQ and leaves the other two Unspecified (code point 2) -- it
    // does NOT, as it once did, also declare Reserved primaries and
    // Identity/GBR matrix because the whole group was copied whenever any
    // member was non-zero.
    //
    // The cost of the sentinel, stated rather than discovered: code point 0
    // itself (Reserved primaries/transfer, Identity/GBR matrix) cannot be
    // REQUESTED through these fields. This encoder converts to YCbCr and has
    // no Identity path to offer, so nothing is lost today; if it ever needs
    // to be requestable it takes a pNext-chained struct with a new sType,
    // per the ABI rules at the top of this header.
    //
    // If NONE of the three is supplied the bitstream carries no colour
    // description at all, which a decoder reads as Unspecified -- not as a
    // declaration of BT.709, and not as RGB.
    //
    // MATRIX, AND WHAT IT SELECTS: on an RGBA input,
    // matrixCoefficients selects the matrix that is APPLIED, so the
    // label the bitstream carries describes the pixels it carries. 1, 5, 6
    // and 9 are taken as named. 2 (Unspecified) is accepted and the matrix
    // is DERIVED FROM colourPrimaries -- 9 from primaries 9, 6 from
    // primaries 5, 6 or 7, and 1 otherwise -- then applied AND signalled
    // back in place of the 2, so a caller that declares BT.2020 primaries
    // and names no matrix gets BT.2020 chroma under a BT.2020 label. 10
    // (BT.2020 constant luminance) is the one accepted-and-approximated
    // code point: it is signalled as asked and converted with the
    // non-constant-luminance matrix, the only BT.2020 derivation available
    // here. A code point that names a matrix the filter cannot produce --
    // 7 (SMPTE 240M), and anything outside BT.709/BT.601/BT.2020 -- is
    // REFUSED at InitializeExt with a reason, not converted as BT.709 under
    // the caller's label. 0 never reaches that gate: on this surface it is
    // the "not supplied" sentinel described above, and is read as
    // Unspecified.
    //
    // THE REFUSAL IS THE RGBA ARM'S ALONE, and the scope is stated because
    // the paragraph above could be read as unconditional. On the DIRECT lane
    // -- a Y'CbCr input the encoder reads as it lies -- this library applies
    // NO matrix, so matrixCoefficients is a LABEL FOR THE CALLER'S OWN
    // SAMPLES and there is nothing to refuse. Any code point the codec can
    // express is carried into the VUI unexamined, including the ones the
    // filter could not have produced. Refusing there would reject a truthful
    // description of a picture this library never touched.
    uint8_t colourPrimaries;
    uint8_t transferCharacteristics;
    uint8_t matrixCoefficients;
    VkBool32 videoFullRange;

    // The transfer function the SUBMITTED FRAMES carry, as an ISO/IEC 23091-4
    // code point. transferCharacteristics above declares the transfer
    // function the ENCODED BITSTREAM advertises, which is the one the encoder
    // works in; this one declares what the input arrives in. Declaring both
    // is how a caller states an OTF requirement rather than implying one.
    //
    // 0 means "not declared" and asserts nothing: the input is taken to be in
    // transferCharacteristics already.
    //
    // THE CONTRACT. This library converts the COLOUR MODEL only -- RGB to
    // YCbCr, and between YCbCr plane layouts and bit depths. It implements NO
    // transfer function and applies none: the code values it writes are the
    // code values it read. So the input and the bitstream must name the same
    // transfer function, and a non-zero inputTransferCharacteristics that
    // differs from transferCharacteristics is REFUSED at InitializeExt
    // (VK_ERROR_INITIALIZATION_FAILED) with the reason. Refusing is the
    // point: an unapplied transfer function produces pixels that are close
    // enough to look plausible and wrong everywhere.
    //
    // WHAT THIS DECIDES ABOUT THE PREPROCESS CONVERSION. A colour-MODEL
    // difference is what the preprocess filter exists to close, and the
    // library engages it on that difference alone -- see inputFormat above.
    // A transfer-function difference is not something the filter can close,
    // so it is refused here instead of being filtered.
    //
    // The refusal of *_SRGB input views follows from the same rule and is
    // enforced by inputFormat: an sRGB view carries an EOTF the sampler
    // applies before the library sees the pixels, i.e. an input transfer
    // function this library cannot honour.
    uint8_t inputTransferCharacteristics;

    // Device selection. -1 is the library's default: the first enumerated
    // device carrying the required device extensions and queue families.
    //
    // 0 IS NOT "UNSET". It is read as a PCI device ID, so a config that
    // reaches this field holding 0 -- a memset, or any initialization that
    // bypasses the default member initializer -- selects nothing and fails
    // VK_ERROR_FEATURE_NOT_PRESENT.
    int32_t deviceId = -1;
    uint8_t gpuUUID[VK_UUID_SIZE]; // Preferred GPU UUID (all zeros = auto)

    // Bitstream output file path (null or empty = encoder library default, e.g. out.264/out.265/out.ivf)
    const char* outputPath;

    // Debug
    VkBool32 verbose;
    VkBool32 validate;          // Vulkan validation layers

    // When VK_TRUE, the encoder does not write to outputPath
    // and instead captures the encoded bitstream in memory; the data is
    // returned via VkVideoEncodeResult::pBitstreamData / bitstreamSize.
    // The caller must copy out before invoking ReleaseEncodedFrame.
    // Default VK_FALSE preserves the original file-output behavior.
    //
    // The COMPLETION surface does not depend on this flag. In both modes
    // every submitted frame raises the completion edge (callback / event
    // handle / counter) and becomes acquirable exactly once. In file-output
    // mode the acquired result is a metadata record: bitstreamSize == 0,
    // pBitstreamData == nullptr, status VK_SUCCESS when the bytes reached
    // the file (the file-write error code otherwise), with isIDR and
    // pictureType describing the frame. Distinguish it from a deadline drop
    // by status (VK_SUCCESS vs VK_TIMEOUT). Release rules are identical:
    // every delivered frame requires ReleaseEncodedFrame, and releasing a
    // still-PENDING frame remains legal (its completion record is then
    // discarded when it arrives).
    VkBool32 disableFileOutput  = VK_FALSE;

    // When VK_TRUE, the encoder library sends its own diagnostic output to a
    // null stream instead of the console. An embedder that runs the encoder
    // inside a sandboxed process, where writing to stdout/stderr is at best
    // lost and at worst trips sandbox diagnostics, sets this VK_TRUE.
    // Latched process-wide at InitializeExt() time; default VK_FALSE.
    //
    // IT DOES NOT SILENCE THE LIBRARY. Only the library's own gated output is
    // covered. Some paths write to stdout and stderr directly and are not
    // intercepted, and those are not all chatter -- the encoder's
    // bitstream-readback failure reports are among them, as is the usage and
    // per-option text written by the argv bridge in vulkan_video_encoder.h.
    //
    // So treat VK_TRUE as "the library stops volunteering status", not as a
    // guarantee that nothing reaches stdout or stderr. An embedder that needs
    // the guarantee has to redirect the descriptors itself.
    VkBool32 silenceStdio       = VK_FALSE;

    // ======================================================================
    // DEVICE OWNERSHIP. Three configurations, and which one you get is
    // decided entirely by which of the three handles below are non-null.
    //
    //   OWN    all three VK_NULL_HANDLE. The library creates the instance,
    //          selects a physical device and creates the logical device.
    //          The only configuration available to a process that has no
    //          Vulkan implementation of its own.
    //
    //   ADOPT  externalInstance + externalPhysicalDevice, externalDevice
    //          LEFT NULL. The library BORROWS the instance, is PINNED to the
    //          supplied physical device, and creates its OWN VkDevice on it,
    //          probing its own queue families. Nothing of the caller's is
    //          destroyed at teardown. This is the supported way for an
    //          embedder that already has Vulkan up to keep the encoder on the
    //          same GPU it composites on without lending it a logical device.
    //
    //   IMPORT all three supplied. The library encodes on the CALLER's
    //          VkDevice and binds the caller's queue families. For an
    //          embedder that must put the encoder on a logical device it
    //          already owns; the context path
    //          (CreateVulkanVideoEncoderExtOnContext) does not take it.
    //
    // PREFER ADOPT TO IMPORT. Under ADOPT the library creates a device it
    // fully specifies while the physical device still comes from the
    // embedder, so landing on the wrong GPU is structurally impossible.
    // Under IMPORT the encoder's queues, enabled extensions and device
    // lifetime are whatever the embedder created for its own purposes.
    //
    // THE PIN IS ENFORCED, NOT ADVISORY. If deviceId or gpuUUID is also set
    // and names something other than the supplied physical device, the
    // library REFUSES (VK_ERROR_FEATURE_NOT_PRESENT out of InitializeExt).
    // It does not fall back to enumerating and picking something else -- a
    // pin that silently re-selects is worse than no pin, because it looks
    // like one.
    // ======================================================================

    // External VkInstance (optional)
    // When non-null, the encoder creates its VkDevice on this instance
    // instead of creating its own instance. Required for cross-process
    // import on Windows where opaque Win32 handles are scoped per-instance,
    // and required for ADOPT.
    //
    // VALIDATION OVER A BORROWED INSTANCE: setting `validate` alongside this
    // does NOT install a debug callback. A debug callback is an
    // instance-level object, and only the party that called vkCreateInstance
    // knows whether VK_EXT_debug_utils / VK_EXT_debug_report was enabled on
    // it. The embedder's own layer and callback report as usual.
    VkInstance       externalInstance                = VK_NULL_HANDLE;

    // Caller-supplied VkPhysicalDevice / VkDevice / queue families.
    // When externalDevice is set, the encoder shares the caller-provided
    // VkDevice instead of creating its own. Nothing the caller supplied is
    // destroyed at teardown, and the instance is tracked separately from the
    // device, which is what makes the ADOPT combination -- borrowed
    // instance, library-owned device -- tear down correctly.
    //
    // Contract:
    //   * externalInstance non-null is allowed standalone (VkInstance can
    //     be shared without sharing the device).
    //   * externalPhysicalDevice non-null with externalDevice VK_NULL_HANDLE
    //     is ADOPT, and is fully supported on this factory path: the library
    //     pins to that physical device and creates its own logical device on
    //     it. A session created on a context already takes both handles from
    //     the context, so naming either one in the config is refused there
    //     instead -- see CreateVulkanVideoEncoderExtOnContext.
    //   * externalDevice non-null REQUIRES externalPhysicalDevice non-null
    //     (Vulkan provides no API to recover the physical device from a
    //     logical device handle). InitializeExt() rejects the mismatched
    //     combination with VK_ERROR_INITIALIZATION_FAILED. On this factory
    //     path this is the only combination of the two that is rejected --
    //     the reverse, physical-device-without-device, is ADOPT.
    //   * Default-initialized VK_NULL_HANDLE / UINT32_MAX preserves the
    //     original behavior (library creates everything / probes families).
    //
    // externalEncodeQueueFamilyIndex / externalComputeQueueFamilyIndex:
    // when externalDevice is set, the caller created the device's queues, so
    // the library MUST bind the families the caller actually created queues
    // for: vkGetDeviceQueue on a family the device was not created with is
    // undefined behavior, and the library's own probe may legally pick a
    // different one. A valid family index here is used verbatim for the
    // encode / compute queue, after validating that the family exists and
    // carries the required queue flags and codec ops. UINT32_MAX (the
    // default; == VK_QUEUE_FAMILY_IGNORED) keeps the library's probed family,
    // and is the only correct value when externalDevice is VK_NULL_HANDLE,
    // where these fields are ignored.
    VkPhysicalDevice externalPhysicalDevice          = VK_NULL_HANDLE;
    VkDevice         externalDevice                  = VK_NULL_HANDLE;
    uint32_t         externalEncodeQueueFamilyIndex  = UINT32_MAX;
    uint32_t         externalComputeQueueFamilyIndex = UINT32_MAX;
};


//=============================================================================
// External Frame Descriptor
//
// Describes a frame to encode that was allocated externally
// (e.g. imported from DMA-BUF in a cross-process encoder service).
//=============================================================================
struct VkVideoEncodeInputFrame {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_FRAME;
    const void*                 pNext = nullptr;

    // The VkImage to encode (must be on the same device as the encoder)
    VkImage  image;

    // Image properties (must match the actual image)
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageTiling imageTiling = VK_IMAGE_TILING_OPTIMAL;  // Must match actual image for path selection
    VkImageLayout currentLayout; // Current layout of the image

    // Frame identification
    uint64_t frameId;           // Unique frame identifier
    uint64_t pts;               // Presentation timestamp (90kHz or custom)

    // Force an IDR at this frame. VK_FALSE lets the GOP structure decide.
    VkBool32 forceIDR;

    // Set to VK_TRUE for the last frame to properly close the GOP
    // and write end-of-stream markers. Without this, decoders may
    // not be able to decode the trailing frames.
    VkBool32 isLastFrame = VK_FALSE;

    // Per-frame QP override (-1 = use session default)
    // Per-frame quantizer, -1 for "use the session's configured constQp".
    //
    // Units are the codec's own QP units -- the same ones the config's
    // constQp uses: 0..51 for H.264/H.265, qindex 0..255 for AV1. There is no
    // second convention to learn.
    //
    // Honoured ONLY when the session was initialized with rate control
    // DISABLED. In CBR/VBR the encoder owns QP and an override would fight its
    // rate controller, so it is refused there and logged once rather than
    // half-applied.
    int32_t qpOverride;

    // A per-frame tag the CALLER owns, for the caller's own diagnostics. The
    // library accepts it and interprets nothing: it does not compare it,
    // index by it, or echo it back. It is the producer's identifier for the
    // IMAGE this frame was submitted from -- typically a slot in its frame
    // pool -- which is a different thing from frameId.
    //
    // -1 is "no tag" and is the default. Every other value is the caller's to
    // define. There is no counterpart on VkVideoEncoderFrameSubmitInfo, where
    // the registration already names the image.
    int32_t uniqueImageIndex = -1;

    // Queue-family ownership of |image| (see the enum above).
    // Leave AUTO for first-use images, whose residency the declared layout
    // infers; set LOCAL for reusable host-written staging images and
    // FOREIGN for dma-buf/pixmap imports.
    //
    // HONOURED AS DECLARED, ON EVERY HANDLE TYPE, WITH ONE LAYOUT
    // QUALIFICATION -- the rule stated in full on
    // VkVideoEncoderExternalImageDescriptor::residency. An explicit LOCAL is
    // taken as given wherever it is made and never derives a foreign acquire.
    // An explicit FOREIGN still defers to a PREINITIALIZED layout. AUTO is
    // derived by that same layout rule. Handle type is declared on the
    // descriptor at registration and plays no part in reading this field.
    //
    // SO THE LAYOUT DECLARATION IS NOT FREE. Changing currentLayout changes
    // the routing, and can turn a local host-written staging image into a
    // queue-family ownership acquire.
    //
    // A REUSED registration does not have to declare a RESTORABLE layout:
    // the library records the layout its own handback left the image in and
    // names that on the next acquire, so a declaration only has to be true
    // on the FIRST frame. See
    // VkVideoEncoderExternalImageDescriptor::defaultLayout.
    VkVideoEncoderInputResidency inputResidency =
        VK_VIDEO_ENCODER_INPUT_RESIDENCY_AUTO;

    // Synchronization: wait semaphores
    // The encoder will wait on these before accessing the image.
    // Typically this is the producer's graph timeline semaphore.
    //
    // BOUNDED at eight on the zero-copy submit path. The other path carries
    // no such bound.
    //
    // A frame this entry point routes DIRECT whose wait list exceeds eight
    // entries is REFUSED with VK_ERROR_TOO_MANY_OBJECTS, returned by the call
    // itself, rather than having a wait silently dropped. Returned BY THE
    // CALL is the load-bearing half: the direct path issues its submit from
    // the deferred-GOP flush, and under B-frame reordering that flush runs on
    // a LATER call, so a refusal reached there would have no route back to
    // whoever holds this frame's status -- a success for a frame that never
    // runs, raises no completion edge, and can only be waited on.
    //
    // Eight is the ceiling on everything the frame carries, not on this field
    // alone: a session encoding with a QP map spends a slot, and so does the
    // hardware load-balancing timeline, so a frame carrying those can be
    // refused below eight declared waits. The registration path publishes the
    // same limit as ERROR_RESOURCE_LIMIT out of SubmitRegisteredFrame.
    uint32_t    waitSemaphoreCount;
    // Const because the library only ever READS these arrays: it copies each
    // element into the submit info it builds and never writes back through
    // them. Declared non-const they would force every caller holding a const
    // array -- which the public submit info hands out -- to cast the
    // qualifier away at the assignment, and a cast that strips const is
    // indistinguishable at the call site from one that intends to write.
    const VkSemaphore* pWaitSemaphores;    // Array of semaphores to wait on
    const uint64_t*    pWaitSemaphoreValues; // Timeline values (0 for binary semaphores)

    // Synchronization: signal semaphores
    // The encoder will signal these after the image is no longer needed.
    // Typically this is the consumer's release timeline semaphore.
    uint32_t    signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;    // Array of semaphores to signal
    const uint64_t*    pSignalSemaphoreValues; // Timeline values (0 for binary)
};

//=============================================================================
// Encoded Frame Result
//
// Returned by GetEncodedFrame() after encoding completes.
//=============================================================================
struct VkVideoEncodeResult {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_ENCODE_RESULT;
    const void*                 pNext = nullptr;

    uint64_t frameId;           // Matches VkVideoEncodeInputFrame::frameId
    uint64_t pts;               // Pass-through from input
    uint64_t dts;               // Decode timestamp (encoder-assigned)

    // Bitstream
    // Valid until ReleaseEncodedFrame(frameId). NOT invalidated by the
    // next retrieval call -- several frames may be held acquired at
    // once. ONE carve-out: encoder teardown (the final release of the
    // encoder object) frees this storage regardless of acquisition
    // state, so a consumer must not hold delivered pointers across the
    // encoder's destruction.
    const uint8_t* pBitstreamData;
    uint32_t bitstreamSize;     // Size in bytes

    // Frame info
    VkVideoEncoderPictureType pictureType;  // I=0 (incl. IDR), P=1, B=2
    VkBool32 isIDR;
    uint32_t temporalLayerId;

    // Encode status: VK_SUCCESS, or the per-frame assembly/readback
    // failure code -- e.g. VK_INCOMPLETE when the encode query status was
    // not COMPLETE (such as INSUFFICIENT_BITSTREAM_BUFFER_RANGE). On
    // failure pBitstreamData is null and bitstreamSize is 0; the frame is
    // still delivered (and must still be ReleaseEncodedFrame()d) so the
    // caller can raise an actionable per-frame error instead of stalling.
    //
    // SIZING A CONSUMER BUFFER NEEDS NO SEPARATE QUERY: the in-memory capture
    // path never truncates, so bitstreamSize already IS the size required for
    // the caller's output buffer. Vulkan video exposes no size query, so a
    // frame that fails this way reports no required size and cannot be
    // re-encoded larger; the per-frame status above is what makes the
    // condition observable and actionable.
    VkResult status;
};

//=============================================================================
// Runtime info descriptor
//
// Caller-queryable snapshot of the encoder's runtime characteristics, for a
// consumer that has to describe the encoder to its own clients.
//
// Carries the fields that decide how a consumer advertises the encoder:
// trusted rate controller, resolution alignment, native-handle and
// hardware-acceleration flags. Per-temporal-layer SVC arrays are not
// reported.
//=============================================================================
struct VkVideoEncoderRuntimeInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_RUNTIME_INFO;
    const void*                 pNext = nullptr;

    char     implementationName[64];
    VkBool32 isHardwareAccelerated;
    VkBool32 supportsNativeHandle;
    VkBool32 trustedRateController;
    VkBool32 supportsSimulcast;
    VkBool32 supportsFrameSizeChange;
    VkBool32 reportsAverageQp;
    uint32_t requestedResolutionAlignmentWidth;
    uint32_t requestedResolutionAlignmentHeight;
    VkBool32 applyAlignmentToAllSimulcastLayers;
};

//=============================================================================
// An imported synchronization primitive, registered once.
//
// The per-frame path takes VkSemaphore handles, which are process-local: a
// producer in another process has nothing meaningful to put there. Import
// once at setup, name by id per frame.
//=============================================================================
typedef struct VkVideoEncoderSemaphoreDescriptor {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_SEMAPHORE_DESCRIPTOR;
    const void* pNext = nullptr;

    // OPAQUE_FD on Linux, OPAQUE_WIN32 on Windows. DMA_BUF and VK_IMAGE are
    // image handle types and are rejected here.
    VkVideoEncoderExternalHandleType handleType =
        VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_NONE;

    // TIMELINE only. A binary semaphore cannot express "wait for frame N" and
    // is single-use, so it cannot be registered once and named repeatedly --
    // the whole point of registering it.
    VkSemaphoreType semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    // Ownership mode for the handle passed to RegisterSemaphore (see
    // VkVideoEncoderHandleOwnership). Defaults to TRANSFER.
    VkVideoEncoderHandleOwnership ownership =
        VK_VIDEO_ENCODER_HANDLE_OWNERSHIP_TRANSFER;
} VkVideoEncoderSemaphoreDescriptor;

//=============================================================================
// Per-frame synchronization, by registered id and value.
//
// Chain onto VkVideoEncoderFrameSubmitInfo::pNext. Present so the per-frame
// path carries INTEGERS only: the handles crossed the process boundary once,
// at registration.
//
// The submit info's raw VkSemaphore arrays remain for in-process callers.
// Where this descriptor NAMES a direction, the registered ids replace that
// direction's raw array outright.
//
// The override is PER DIRECTION. A chain that names only waits leaves the
// caller's signal array exactly as it found it, and the mirror image holds
// too.
//=============================================================================
typedef struct VkVideoEncoderFrameSyncDescriptor {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_SYNC_DESCRIPTOR;
    const void* pNext = nullptr;

    uint32_t                            waitCount = 0;
    const VkVideoEncoderResource*       pWaitSemaphores = nullptr;
    const uint64_t*                     pWaitValues = nullptr;

    uint32_t                            signalCount = 0;
    const VkVideoEncoderResource*       pSignalSemaphores = nullptr;
    const uint64_t*                     pSignalValues = nullptr;
} VkVideoEncoderFrameSyncDescriptor;

//=============================================================================
// Per-frame fences (Linux sync_fd). Chain onto
// VkVideoEncoderFrameSubmitInfo::pNext -- the SAME chain the sync descriptor
// above hangs off, never a sub-chain of it.
//
// The walk is a FLAT, ORDER-INDEPENDENT pNext list, per the chaining rules at
// the head of this header. All three of these are honoured identically, and
// an unknown sType anywhere in the chain is refused wherever it sits:
//
//     info.pNext = &fence;                                  // fence alone
//     info.pNext = &fence;   fence.pNext = &sync;           // fence leading
//     info.pNext = &sync;    sync.pNext  = &fence;          // fence trailing
//
// Separate from RegisterSemaphore, which is timeline-only: a binary semaphore
// cannot express "wait for frame N" and cannot be registered once then named
// repeatedly, which is the entire point of registering it. But a cross-API
// compositor's sync currency IS binary -- a fence handle on Linux is a binary
// sync_fd -- so a binary fence travels per frame.
//
// A SYNC_FD payload is consumed by the FIRST wait, which is exactly what a
// per-frame fence wants and why a registered semaphore is not imported the
// same way.
//=============================================================================
typedef struct VkVideoEncoderFrameFenceDescriptor {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_FENCE_DESCRIPTOR;
    const void* pNext = nullptr;

    // A fence the encoder waits on before reading the input image. -1 = none.
    // The library takes ownership and closes it on every exit path. On Win32
    // this field is not used: the value there is not a file descriptor, the
    // library imports nothing from it, and it closes nothing -- a Win32 caller
    // must leave this at -1.
    //
    // CALLER OBLIGATION, AND THE LIBRARY CANNOT DISCHARGE IT FOR YOU.
    // After ANY return from SubmitRegisteredFrame -- SUCCESS or any refusal --
    // the number still sitting in this field names a descriptor that is
    // already gone. The library does NOT write -1 back here -- unlike
    // pReleaseFenceFd below, which it writes through a pointer into the
    // caller's own storage.
    //
    // So a caller that KEEPS this descriptor and submits it again -- parking a
    // frame that was refused and retrying it later is the obvious shape, and
    // NOT_READY makes it the expected one -- MUST re-arm this field with a
    // freshly exported fd, or set it to -1, before the second call.
    // Re-submitting the identical struct hands the library a stale number: it
    // is closed a second time, and by then the process may have opened
    // something unrelated at that number, so the victim is not the fence.
    //
    // ADDITIVE TO THE RAW WAIT ARRAY, not a replacement for it -- which is
    // where this differs from the sync descriptor above, whose named
    // direction replaces that direction outright. A sync_fd is a SINGLE fence
    // object, so a caller that exported one of its producers as an fd and
    // left the rest in pWaitSemaphores has said something the fence alone
    // cannot say. The library therefore waits on BOTH: supplying an
    // acquireFenceFd and a pWaitSemaphores array together is legal and loses
    // neither.
    //
    // BOUNDED ON THE DIRECT PATH, which qualifies "loses neither" above:
    // the direct submit assembles its waits into a fixed eight-entry array.
    // A registration that routes DIRECT must satisfy
    //     waitSemaphoreCount + (acquireFenceFd >= 0 ? 1 : 0) <= 8
    // -- this fence is appended to the wait array BEFORE the count is
    // checked, so it is counted. Above eight, SubmitRegisteredFrame REFUSES
    // the frame with VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT instead
    // of dropping the surplus. Seven caller waits plus this fence is eight
    // and fits. A registration that is not zero-copy carries no such bound,
    // so this is a property of the direct arm alone.
    //
    // The refusal does NOT hand this fd back: it is imported before the
    // count is checked, so it is consumed on that exit exactly as on every
    // other. The ownership rule above is not suspended by the refusal.
    int   acquireFenceFd = -1;

    // [out, optional] A binary SYNC_FD fence signalled when the encoder has
    // finished READING the input image. That is the submission which CONSUMES
    // the input, and NOT the encode's completion: the bitstream becoming
    // retrievable is a different event on a different resource, and the two
    // must not be conflated.
    //
    // Feed it into the access seam that owns the backing, as a READ fence.
    // The library never discharges that obligation for you; it only hands you
    // the input to it.
    //
    // OWNERSHIP: the exported fd is the CALLER's. The library never closes
    // it. close(2) it, or hand it to exactly one import, which consumes it.
    // This is the REVERSE of acquireFenceFd above, and of every other fd rule
    // in this header -- all of which govern handles the library is GIVEN.
    //
    // WHEN IT IS WRITTEN: before anything in this call can refuse. A caller
    // may declare `int fd;` and rely on it being defined on EVERY return --
    // including ERROR_IMPORT_FAILED, ERROR_RESOURCE_UNKNOWN and
    // ERROR_STRUCTURE_TYPE_UNKNOWN, and including the chains that put the
    // refusing node AHEAD of this descriptor. A successful export overwrites
    // it after the submit.
    //
    // -1 means "no fence", NOT failure, and is a legal answer every caller
    // must handle. It is returned when: the platform or device cannot export
    // a SYNC_FD binary semaphore; the frame already carries four or more
    // signal semaphores, which leaves no room for this fence in the direct
    // submit's signal array (refusing the fence is preferred to handing out
    // an fd nothing will signal); the input-consuming submission had not been
    // issued by the time this call returned (a frame deferred into a B-frame
    // reorder batch is submitted on a LATER call); or the fence was already
    // signalled, which vkGetSemaphoreFdKHR itself reports as -1. On -1 the
    // caller must fall back to its own ordering -- it must NOT read -1 as
    // "the input is already released".
    int*  pReleaseFenceFd = nullptr;
} VkVideoEncoderFrameFenceDescriptor;

//=============================================================================
// Answer to QueryImageSupport. Chain a VkVideoEncoderImageSupportDetails
// (below) onto pNext for the renegotiation modifiers; unknown chained sTypes
// are rejected.
//=============================================================================
typedef struct VkVideoEncoderImageSupport {
    VkVideoEncoderStructureType sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT;
    const void*                 pNext = nullptr;

    // Would RegisterImageResource accept this descriptor?
    VkBool32 supported = VK_FALSE;

    // Why not, when supported is VK_FALSE. VK_VIDEO_ENCODER_STATUS_SUCCESS
    // when it is VK_TRUE.
    VkVideoEncoderStatusCode status = VK_VIDEO_ENCODER_STATUS_SUCCESS;
} VkVideoEncoderImageSupport;

// Capacity of the renegotiation list below. Truncated silently past this:
// the list exists so a producer can pick SOME workable allocation, not to
// enumerate the device.
#define VK_VIDEO_ENCODER_MAX_DIRECT_MODIFIERS 32

//=============================================================================
// Optional extension of VkVideoEncoderImageSupport: chain onto its pNext to
// receive, alongside the verdict:
//
//   * directModifiers -- when the descriptor names an OS-handle import, the
//     DRM format modifiers with which this exact descriptor (same format,
//     usage, flags, extent; modifier swapped) WOULD register. This is the
//     renegotiation list MODIFIER_UNSUPPORTED refers to: a producer
//     re-allocates with one of these and resubmits -- one round trip.
//     Count 0 when not initialized, when the descriptor is not an OS-handle
//     import, when the descriptor declares no usage (imageUsage 0 is
//     refused as USAGE_INSUFFICIENT under ANY modifier, so there is
//     nothing to renegotiate), or when the device supports no workable
//     modifier.
//=============================================================================
typedef struct VkVideoEncoderImageSupportDetails {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMAGE_SUPPORT_DETAILS;
    const void*                 pNext = nullptr;  // MUST be NULL

    uint32_t directModifierCount = 0;                                     // OUT
    uint64_t directModifiers[VK_VIDEO_ENCODER_MAX_DIRECT_MODIFIERS] = {}; // OUT
} VkVideoEncoderImageSupportDetails;

//=============================================================================
// Per-call status echo. The library REPORTS which
// ownership rule it applied, so the caller can assert rather than guess --
// it is never a control input. handlesConsumed is VK_TRUE iff the CALLER's
// handle was consumed by the call it was passed to: always true for a
// POSIX fd type under TRANSFER (success or failure alike), always false
// under BORROW (the library only ever consumes its private duplicate),
// always false for Win32 handles and for VK_IMAGE (no handle is taken).
// A constant of (platform, handle type, mode) -- deliberately not of the
// call's outcome, which is the whole point.
//
// pNext CARRIES A CHAIN ONLY WHERE THERE IS A VERDICT TO DELIVER, which is
// RegisterImageResource. There it accepts at most one
// VkVideoEncoderImportGuardInfo and at most one
// VkVideoEncoderImportContentInfo (both below), in either order; an unknown
// link, or a REPEATED link of either known type, is refused as version skew,
// with the ownership rule still applied to the handle on that exit. Two
// known types are consumed and everything else refused, because an
// extension the library does not understand means the caller asked for
// something it is not getting, and silently ignoring it is the one outcome
// this gate exists to prevent.
//
// On RegisterSemaphore the echo takes NO chain. Both verdict structs report
// on a dma-buf IMAGE import, and a semaphore registration performs none, so
// any non-null pNext there is version skew and is refused -- with the
// ownership rule applied on that exit like every other, which under
// TRANSFER means the fd is consumed by the refusal.
//=============================================================================
struct VkVideoEncoderStatus {
    VkVideoEncoderStructureType sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
    const void*                 pNext = nullptr;

    VkBool32 handlesConsumed = VK_FALSE;  // OUT
};

//=============================================================================
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
// reports DISABLED for every dma-buf import inside the guard's scope.
// Outside that scope -- a non-NVIDIA device -- the out-of-scope verdict is
// reported instead, so read |state| rather than assuming DISABLED.
// VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD in the environment disables the
// guard in a build that enables it.
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
// every path, so a caller compares against it rather than hard-coding it.
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
//=============================================================================
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

//=============================================================================
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
// yet carried a frame. A caller reacts to a damaged verdict by no longer
// using that registration.
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
// registration, and the caller's reaction is not destructive.
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
//                    this on success.
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
// the capture pool cannot serve that format and
// extent. An ARMED echo is therefore a verdict PENDING and not a verdict
// promised; armedRegistrationCount tells one still in flight from one that
// will never arrive.
//
// probeGeneration is a non-zero BUILD CONSTANT
// (VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION) stamped on every path,
// so probeGeneration != 0 is proof the library wrote the struct.
//
// meanY / meanU / meanV are the plane means in Q8 FIXED POINT -- the 0..255
// mean times 256, so 128.0 reads as 32768 and the predicate's "< 2" is
// "< 512". Integers rather than floats because this struct crosses an ABI.
// They are filled on CLEAN and DAMAGED_* alike, so a caller can log why.
//=============================================================================

// Non-zero by contract: a non-zero probeGeneration is what proves the
// library wrote the struct at all. Bump it if the predicate or the sampling
// changes in a way that makes old and new verdicts non-comparable.
#define VK_VIDEO_ENCODER_IMPORT_CONTENT_PROBE_GENERATION 1u

// The predicate's threshold, in the Q8 units meanY/meanU/meanV carry: a plane
// mean strictly below 2.0/255. Named rather than open-coded because the
// library's scorer and every consumer's assertion have to agree on it.
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

//=============================================================================
// Extended Encoder Interface
//
// Extends VulkanVideoEncoder with external frame input and sync support.
// The base VulkanVideoEncoder methods (Initialize, EncodeNextFrame, etc.) are
// the file-based encoding interface -- declared in vulkan_video_encoder.h,
// which ships alongside this header and carries its own factory. This class
// adds to that interface; it does not replace it.
//=============================================================================
class VulkanVideoEncoderExt : public VulkanVideoEncoder {
public:
    // Initialize with structured config (alternative to argc/argv)
    virtual VkResult InitializeExt(const VkVideoEncoderConfig& config) = 0;

    // === Direct image submit: a VkImage per frame, without a registration ===
    //
    // Hand over a VkImage per frame rather than naming a registration.
    // Prefer RegisterImageResource + SubmitRegisteredFrame for new code:
    //   - the import, allocation and validation happen once per producer pool
    //     slot instead of once per frame;
    //   - an unsupported format or DRM modifier fails ONCE, at a negotiation
    //     point, with a code the caller can act on -- rather than on every
    //     frame forever, mid-stream, as a driver-level error;
    //   - a uint64 registration id crosses an IPC boundary; a VkImage handle
    //     does not, so a cross-process caller has no alternative.
    //
    // WHEN THE FRAME'S SEMAPHORES FIRE. The encoder waits on the frame's
    // wait semaphores before it reads the input image, and signals the
    // frame's signal semaphores once it has FINISHED READING that image.
    // That is the RELEASE POINT. It is not the same event as the bitstream
    // becoming retrievable, which is collected separately; on the DIRECT
    // path below both are produced by the one encode submission, so a
    // release carries no implication that the bitstream is not yet ready.
    //
    // Two input paths, chosen by the library from the frame's format and
    // tiling and not something the caller asks for. They differ in one thing
    // the caller can observe, which is WHERE that release point falls:
    //   DIRECT -- the image is encoded as it stands, so the release point is
    //     the encode submission itself.
    //   STAGED -- everything else. The input is taken into an image of the
    //     library's own first, and the release point is the completion of
    //     that staging copy, which is earlier than the encode.
    //
    // THIS CALL IS NOT ASYNCHRONOUS: the whole CPU-side encode pipeline runs
    // INLINE on the calling thread before it returns, so budget for a full
    // CPU-side encode issue rather than an enqueue. It does not block on
    // capacity -- a full queue is refused up front as VK_NOT_READY, before
    // any state changes. What IS asynchronous is completion: the bitstream is
    // retrieved with AcquireNextEncodedFrame (or GetEncodedFrame).
    // Threading: class (b), below.
    //
    // pStagingCompleteSemaphore [out, optional]: if non-null, receives the
    //   binary semaphore signaled when the staging copy completes. Useful
    //   when the caller needs to chain further GPU work (e.g. a display blit)
    //   that reads the same external image and must know when the encoder is
    //   done reading it.
    //
    //   On the DIRECT path there is no staging copy, so VK_NULL_HANDLE is
    //   written. The frame's own signal semaphores, signaled by the encode
    //   submission, are the release point there.
    //
    //   Signal semaphores passed in the frame are signaled at that same
    //   point. If the release must happen AFTER additional work, do not pass
    //   signal semaphores in the frame; chain on pStagingCompleteSemaphore
    //   and signal the release semaphore from the final submission.
    //
    // Returns VK_SUCCESS if the frame was accepted for encoding.
    // Returns VK_NOT_READY if the encoder's queue is full (retry later) --
    // flow control, not an error.
    virtual VkResult SubmitExternalFrame(
        const VkVideoEncodeInputFrame& frame,
        VkSemaphore* pStagingCompleteSemaphore = nullptr) = 0;

    // === Asynchronous Bitstream Retrieval ===
    //
    // After a submit RETURNS, the GPU execution and the bitstream capture
    // complete asynchronously; the submit call itself is not
    // asynchronous (see the class (b) warning below). Use these methods
    // to retrieve the encoded bitstream without blocking the encode
    // pipeline.

    // THREADING CONTRACT (whole interface):
    //   (a) session-serial -- one thread, never concurrent with each
    //       other: InitializeExt, Flush, DrainPendingFrames,
    //       Reconfigure, SetCompletionCallback. ENFORCED, not merely
    //       documented: each returns VK_ERROR_NOT_PERMITTED_KHR when
    //       called from inside the completion callback. Teardown is in
    //       the same class and has no way to return an error, so
    //       dropping the LAST encoder reference from inside the
    //       callback is a diagnosed abort rather than a silent
    //       self-join or use-after-free.
    //   (b) submit-thread-affine -- the same thread for the whole session:
    //       SubmitExternalFrame, RegisterImageResource,
    //       SubmitRegisteredFrame, UnregisterImageResource,
    //       RegisterSemaphore, UnregisterSemaphore. A submit is
    //       NOT asynchronous -- the whole CPU-side encode pipeline
    //       (GOP/DPB bookkeeping, command recording and the queue
    //       submission) runs inline on the calling thread before it
    //       returns. It does not BLOCK on capacity (that is an up-front
    //       not-ready refusal -- VK_NOT_READY, or
    //       VK_VIDEO_ENCODER_STATUS_NOT_READY from the typed-status
    //       surface -- never a condition-variable wait); what is
    //       asynchronous is completion, which the class (c) surface
    //       below delivers.
    //   (c) fully thread-safe -- any thread: the retrieval methods below
    //       (AcquireNextEncodedFrame, AcquireEncodedFrame, GetEncodedFrame,
    //       ReleaseEncodedFrame), GetFrameStatus, CancelFrame,
    //       CancelAllPendingFrames, AbandonAllFrames, SetFrameDeadline,
    //       GetCompletionCounter, GetCompletionInfo,
    //       GetCompletionEventHandle, GetCompletionSemaphore,
    //       ExportCompletionSemaphoreHandle, SupportsFormat,
    //       GetMaxWidth/GetMaxHeight, GetRuntimeInfo.

    // Get the next completed encoded frame in COMPLETION order -- the order
    // outcomes became deliverable, not the order frames were submitted, so a
    // frame that never completes cannot strand the frames behind it. With no
    // GOP reorder the two coincide, which is why delivery stays monotonic for
    // muxers. Returns VK_SUCCESS and fills |result|; the frame is
    // then ACQUIRED and pBitstreamData stays valid until
    // ReleaseEncodedFrame() for that frameId -- the ONE lifetime rule.
    // Its single carve-out is encoder teardown: the final release of
    // the encoder object invalidates every outstanding pBitstreamData,
    // so release delivered frames before dropping the last encoder
    // reference.
    // Flush() does NOT invalidate them; nothing else does either.
    // Returns VK_NOT_READY while no undelivered frame has either a capture
    // or an expired deadline.
    // A frame past frameCompletionTimeoutNs is delivered as a 0-byte drop
    // with status VK_TIMEOUT: data loss, not session loss -- head-of-line
    // blocking is bounded by the deadline instead of wedging forever.
    // In file-output mode completed frames deliver as 0-byte VK_SUCCESS
    // records (see disableFileOutput).
    // Threading: class (c).
    virtual VkResult AcquireNextEncodedFrame(VkVideoEncodeResult& result) = 0;

    // Keyed retrieval: deliver a SPECIFIC frame regardless of submit order
    // (the caller opts into out-of-order delivery). Returns VK_NOT_READY
    // while pending; VK_ERROR_NOT_PERMITTED_KHR when already acquired;
    // VK_ERROR_UNKNOWN when never submitted or already released.
    // Threading: class (c).
    virtual VkResult AcquireEncodedFrame(uint64_t frameId,
                                         VkVideoEncodeResult& result) = 0;

    // Frame-state query; cannot fail. Threading: class (c).
    virtual VkVideoEncoderFrameState GetFrameStatus(uint64_t frameId) = 0;

    // Alias of AcquireNextEncodedFrame(), for a drain loop written in these
    // terms. Identical behaviour, and neither spelling is preferred over the
    // other. Threading: class (c).
    virtual VkResult GetEncodedFrame(VkVideoEncodeResult& result) = 0;

    // Release a delivered frame: frees the bitstream storage and returns
    // the pool node. Required for EVERY delivered frame, including
    // VK_TIMEOUT drops and per-frame failures. Releasing a still-PENDING
    // frame is also legal (see disableFileOutput): the frame's completion
    // record is discarded silently when it arrives, and does not count
    // toward lateCaptures. Unknown ids are a logged
    // no-op. Threading: class (c).
    virtual void ReleaseEncodedFrame(uint64_t frameId) = 0;

    // Register (or clear, with nullptr) the completion callback -- see the
    // PFN typedef's contract. The callback is invoked on a library thread;
    // invocations are serialized. This call is a QUIESCE POINT: it returns
    // only after any in-flight invocation of the previous
    // callback has returned, so once a nullptr detach returns the caller
    // may safely destroy whatever the previous pUserData referenced. Not
    // callable from inside the completion callback itself
    // (VK_ERROR_NOT_PERMITTED_KHR). Threading: class (a).
    // |releaseUserData|, when non-null, transfers ownership of |pUserData|
    // to the encoder: it is invoked exactly once when the encoder is done
    // with it, after any in-flight invocation has returned. Prefer it to
    // keeping the cookie alive by arrangement with teardown order.
    virtual VkResult SetCompletionCallback(
        PFN_vkVideoEncoderCompletionCallback callback, void* pUserData,
        PFN_vkVideoEncoderUserDataRelease releaseUserData = nullptr) = 0;

    // Monotonic count of frame completions made retrievable (in
    // file-output mode these are metadata-only records). Because callback
    // notifications coalesce, consumers drain until their own retrieval
    // count matches this counter. Threading: class (c).
    virtual uint64_t GetCompletionCounter() = 0;

    // Observability snapshot; |pInfo| must be value-initialized
    // (self-stamped). Chain a VkVideoEncoderDiagnosticInfo to
    // |pInfo|->pNext to read the misuse channel in the same snapshot; a
    // chained struct the library does not recognize is refused
    // (VK_ERROR_INITIALIZATION_FAILED), not ignored -- the walk rule
    // every other entry point applies. Threading: class (c).
    virtual VkResult GetCompletionInfo(VkVideoEncoderCompletionInfo* pInfo) = 0;

    // OS-handle completion currency, for a consumer that cannot be handed a
    // C function pointer -- which is every out-of-process consumer.
    //
    // Returns a handle the caller waits on. The handle is the platform's
    // own: on Linux, an eventfd. No build ships a handle nothing signals, so
    // a caller never has to branch on a handle that exists but is never
    // raised.
    //
    // VK_VIDEO_ENCODER_STATUS_SUCCESS with |*outHandle| set on success;
    // ERROR_HANDLE_TYPE_UNSUPPORTED when the adapter could not create one
    // (|*outHandle| is then 0, which is a legal handle value elsewhere and
    // must not be waited on); ERROR_STRUCTURE_TYPE_UNKNOWN when |outHandle|
    // is null.
    //
    // The handle is created on first request, owned by the encoder, and
    // valid for the LIFETIME OF THE ENCODER OBJECT: it survives session
    // teardown and is closed by the destructor, last, after the worker
    // join (teardown is the refcounted release of the encoder; there is
    // no public Deinitialize). The caller MUST NOT close it. Requesting
    // it twice returns the same handle rather than a second one.
    //
    // Semantics match the callback exactly, and for the same reason:
    // notifications COALESCE. The handle says "at least one frame became
    // ready", never "exactly one". A waiter drains in a loop and reconciles
    // against GetCompletionCounter(), and a waiter that assumes one wake
    // equals one frame will silently strand frames -- the symptom of which
    // looks exactly like a library stall.
    //
    // Registering no callback and using only this handle is a supported
    // configuration; so is using neither.
    virtual VkVideoEncoderStatusCode GetCompletionEventHandle(uint64_t* outHandle) = 0;

    // Completion currency 3: a Vulkan timeline, for GPU ordering ONLY --
    // never a wakeup.
    //
    // A library-owned timeline semaphore, signaled ON THE GPU by the encode
    // queue. Its counter reaching (frameId + 1) means the encode GPU work
    // of every external frame submitted with an id <= frameId has retired
    // on the device: the bitstream and reconstruction writes are visible to
    // device work that waits on it. Signals are COALESCED with the
    // running maximum, because encode order is not input order under
    // B-frames and a timeline may not signal non-monotonically. The +1
    // exists because a timeline's initial value is 0 and frame ids may
    // legally start at 0. Consumers of this currency MUST submit
    // monotonically increasing frameIds.
    //
    // What it is NOT: a readiness or wakeup signal. It is not pollable (an
    // OPAQUE_FD export's legal operations are dup/dup2/close -- not poll),
    // and it says nothing about RETRIEVABILITY: the host-side capture
    // (fence wait, query readback, byte copy) happens after this signal
    // fires, so a frame whose encode has retired here may not yet be
    // acquirable. Readiness is the completion callback and the OS event
    // handle, exclusively.
    //
    // A frame whose submit FAILS never advances the counter; do not enqueue
    // waits for it. A deadline-synthesized VK_TIMEOUT drop is a host-side
    // delivery event and is independent of this counter, in both
    // directions: the counter advances iff the GPU work retires.
    //
    // The encoder owns the semaphore: valid from InitializeExt success
    // until Flush() -- terminal for the encode session, it releases the
    // underlying encoder and this semaphore with it -- or encoder
    // teardown, whichever comes first. VK_NULL_HANDLE outside that
    // window (including the rare session whose creation failed -- the
    // currency degrades, the session does not), never destroyed by the
    // caller. Do not leave GPU waits enqueued on it past that window.
    // Threading: class (c).
    virtual VkSemaphore GetCompletionSemaphore() const = 0;

    // Export the completion timeline for cross-process GPU ordering.
    //
    // OPAQUE_FD only; the Win32 arm is reserved and refused by type until
    // its milestone, like every other Win32 arm here. Each call mints a NEW
    // fd via vkGetSemaphoreFdKHR, and -- the REVERSE of the registration
    // rule, which governs handles GIVEN TO the library -- the CALLER owns
    // this fd: close it, or hand it to exactly one vkImportSemaphoreFdKHR,
    // which consumes it.
    //
    // The export exists so another process can enqueue GPU waits against
    // encode completion. It is not pollable and MUST NOT be a peer's
    // liveness or wakeup mechanism; peer death belongs to the transport:
    // an OPAQUE_FD timeline is never force-signalled when its owner dies,
    // so an obligation that must survive the peer belongs in the
    // transport's own currency, not on this semaphore.
    // This interface carries no sync-file handle type; its semaphore arms
    // import and export OPAQUE_FD timelines only.
    //
    // Returns ERROR_HANDLE_TYPE_UNSUPPORTED when the physical device
    // cannot export an OPAQUE_FD timeline (the semaphore still works
    // in-process) and for the reserved Win32 arm -- one answer, one clean
    // branch, same as a reserved arm anywhere else here. NOT_INITIALIZED
    // before InitializeExt. Threading: class (c).
    virtual VkVideoEncoderStatusCode ExportCompletionSemaphoreHandle(
        VkVideoEncoderExternalHandleType handleType,
        uint64_t* outHandle) = 0;

    // Change the per-frame completion deadline mid-session.
    //
    // The deadline also arrives via the session config, which is read once at
    // InitializeExt -- so without this a caller could not react to anything
    // it learned after starting, which is exactly when a deadline turns out
    // to be wrong.
    //
    // Clamped to stay strictly above the encoder's internal fence wait; a
    // deadline at or below it would fire on the encoder's own normal
    // latency and turn healthy frames into drops. The clamped value is what
    // takes effect, and it is logged when it differs from the request.
    //
    // 0 restores the configured default. Threading: class (c).
    virtual VkResult SetFrameDeadline(uint32_t deadlineMs) = 0;

    // Cancel DELIVERY of a frame: an unacquired frame (pending or ready)
    // is converted to a 0-byte drop with status VK_INCOMPLETE; a late
    // capture is discarded. The GPU work itself is not recalled. Already-
    // acquired frames return VK_ERROR_NOT_PERMITTED_KHR; unknown ids
    // VK_ERROR_UNKNOWN. Cancelled frames still require
    // ReleaseEncodedFrame. Threading: class (c).
    virtual VkResult CancelFrame(uint64_t frameId) = 0;

    // Cancel delivery of every unacquired frame; the count is returned via
    // |pCancelledCount| (may be null). Threading: class (c).
    virtual VkResult CancelAllPendingFrames(uint32_t* pCancelledCount) = 0;

    // Peer loss: abandon every unacquired frame WITHOUT requiring anyone to
    // retrieve and release it, and drop the claims those frames hold on
    // their registrations.
    //
    // Cancel is for a live consumer changing its mind, and keeps the
    // requirement to collect what it cancelled. This is for a consumer that
    // is gone -- an out-of-process peer whose channel dropped -- where
    // nothing will ever call ReleaseEncodedFrame, so that same requirement
    // would pin every registration those frames name for the encoder's
    // remaining lifetime and leave a deferred UnregisterImageResource
    // permanently deferred.
    //
    // ALREADY-ACQUIRED frames are left alone. Their bitstream pointers are
    // out in the caller's hands; freeing underneath a holder is worse than
    // holding memory a while longer, and on a peer-loss path the holder is
    // usually the thing being torn down anyway.
    //
    // |pAbandonedCount| may be null. Threading: class (c).
    virtual VkResult AbandonAllFrames(uint32_t* pAbandonedCount) = 0;

    // === Flush and Drain ===

    // Flush: encode all pending frames and make their bitstreams available.
    // Blocks until all pending frames are encoded.
    //
    // TERMINAL for the encode session: the underlying encoder is released
    // on the way out, so subsequent submits are refused
    // (VK_ERROR_NOT_PERMITTED_KHR / ERROR_NOT_INITIALIZED) and
    // GetCompletionSemaphore() answers VK_NULL_HANDLE from then on.
    // Retrieval and ReleaseEncodedFrame of already-submitted frames stay
    // valid -- delivered bitstream pointers are NOT invalidated by Flush.
    // Use DrainPendingFrames() below for the non-terminal drain.
    // Threading: class (a).
    virtual VkResult Flush() = 0;

    // Non-terminal: flush the deferred GOP tail and wait for all in-flight
    // encodes to complete, WITHOUT releasing the encoder. After this,
    // GetEncodedFrame() can retrieve every frame submitted so far.
    //
    // NON-TERMINAL MEANS THE COMPLETION SURFACE TOO, not just the encoder
    // object. Unlike Flush(), the session stays fully usable: further
    // SubmitExternalFrame/SubmitRegisteredFrame calls are accepted AND each
    // of those frames raises the completion edge and becomes acquirable
    // exactly once, in both output modes, exactly as it would have without
    // the drain. A drain may be called any number of times.
    //
    // Returns VK_ERROR_NOT_PERMITTED_KHR when there is no session or when
    // called from inside a completion callback. A failure return other than
    // that one means the drain completed -- everything already submitted is
    // encoded and retrievable -- but the session could not be brought back
    // up, so it can no longer report completions and further submits will
    // fail rather than silently stall.
    // Threading: class (a).
    virtual VkResult DrainPendingFrames() = 0;

    // === Dynamic Reconfiguration ===

    // Change rate control mid-stream without a session reset. Takes effect at
    // the NEXT ENCODED FRAME, not at an IDR boundary.
    //
    // WHAT IT CHANGES: averageBitrate, maxBitrate, frameRateNum,
    // frameRateDen, the constant-QP defaults constQpI/constQpP/constQpB,
    // and the quantizer clamps minQp/maxQp. pNext must be NULL -- a chain
    // is refused (VK_ERROR_INITIALIZATION_FAILED) -- so a caller that
    // shares one VkVideoEncoderConfig with InitializeExt must clear pNext
    // first.
    //
    // ZERO IS NOT "LEAVE THIS ALONE" ON THREE OF THOSE FOUR:
    //   * averageBitrate of 0 FAILS THE WHOLE CALL with
    //     VK_ERROR_NOT_PERMITTED_KHR, before any of the four is applied, so
    //     a caller moving only the frame rate must still restate the
    //     bitrate already in force.
    //   * maxBitrate of 0 is COERCED TO averageBitrate rather than meaning
    //     "no cap": a session reconfigured with a zero here comes back
    //     capped at its own average.
    //   * frameRateDen of 0 is coerced to 1 when frameRateNum is non-zero.
    //   * frameRateNum of 0 is the one that does mean "unchanged": the
    //     frame rate is left as it was and the call still succeeds.
    //
    // WHAT IS IMMUTABLE FOR THE LIFE OF THE SESSION, and is REFUSED rather
    // than ignored when it differs from what InitializeExt was given: codec,
    // profile, encodeWidth, encodeHeight, inputFormat, inputColorModel,
    // rateControlMode, colourPrimaries, transferCharacteristics,
    // inputTransferCharacteristics, matrixCoefficients and videoFullRange.
    // Each is settled either in the sequence header written once at
    // InitializeExt or in the input routing the session was built around, so
    // changing one needs a session re-init. inputColorModel is compared AS IT
    // RESOLVES against inputFormat: re-spelling the model a format already
    // carries is accepted, while a declaration resolving to the other model
    // is not.
    //
    // WHAT IS ALSO REFUSED ON A CHANGE -- the encoding parameters this call
    // cannot carry. Each one is settled at InitializeExt and can reach the
    // bitstream, so answering VK_SUCCESS to a change would leave the
    // session encoding one way while the caller believed another:
    //
    //   inputWidth and inputHeight -- what the session input conversion was
    //   built around, the other half of a declaration whose format and
    //   colour model are refused above.
    //
    //   vbvBufferSize -- what a command carries is a VBV duration in
    //   milliseconds computed from this AND from an initial delay derived
    //   from the buffer size at init, both against the bitrate at init.
    //   Moving one of the three alone describes a buffer nobody has.
    //
    //   gopLength, consecutiveBFrames, idrPeriod, closedGop -- these drive
    //   the structure that sequences frame types and DPB references as well
    //   as rate control, so a half-applied change would tell the driver one
    //   GOP while the encoder sequenced another.
    //
    //   qualityLevel and tuningMode -- baked into the video session
    //   parameters at creation and into the profile respectively.
    //
    // MINQP AND MAXQP ARE APPLIED, with three exceptions that are REFUSED
    // rather than ignored, because on each of them the value would reach
    // nothing: on an AV1 session (AV1 rate control is quantizer-index based
    // and consumes no QP-unit clamp), on a constant-QP session (that mode
    // takes its quantizer from constQpI/P/B, which this call does carry),
    // and for a value outside the H.26x range 0..51, an inverted window, or
    // a value outside the QP window the device reports. A clamp of 0 means
    // "no clamp", the same reading InitializeExt gives it, so a clamp set
    // through this call can also be cleared through it.
    //
    // WHAT IS NEITHER APPLIED NOR REFUSED. The init-time plumbing and the
    // diagnostics, which are READ BY NOTHING HERE and answered VK_SUCCESS
    // whatever they hold:
    //
    //   deviceId, gpuUUID, outputPath, verbose, validate, disableFileOutput,
    //   silenceStdio, externalInstance, externalPhysicalDevice,
    //   externalDevice, externalEncodeQueueFamilyIndex,
    //   externalComputeQueueFamilyIndex.
    //
    // Not one of them can change an encoded bit, so not one can misdescribe
    // the stream. Refusing them would buy no correctness and would break a
    // caller that builds a fresh minimal config for the reconfigure rather
    // than copying its stored one.
    //
    // A CONSTANT-QP SESSION HAS EXACTLY ONE SESSION-LEVEL LEVER HERE, and it
    // is the constant-QP triple. rateControlMode is immutable, so a session
    // initialized VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR stays
    // constant-QP, and the four rate fields land in per-layer state such a
    // session does not carry -- that mode commands layerCount 0. constQpI,
    // constQpP and constQpB are applied instead: a NEGATIVE member means the
    // config names no quantizer and that one is left alone, while 0 is a
    // valid (lossless) QP and is applied as one. Per-frame,
    // VkVideoEncoderFrameSubmitInfo::qpOverride still moves the rate as well.
    virtual VkResult Reconfigure(const VkVideoEncoderConfig& config) = 0;

    // === Capability Query ===

    // Query encoder capabilities.
    //
    // SupportsFormat answers for the YCbCr formats this library can route. It
    // is DEVICE-BLIND -- it reports what the library accepts, not what the
    // physical device advertises -- and is safe to call before
    // InitializeExt(), but it is NOT SESSION-BLIND.
    //
    // Two classes of format, and the second is why: the 8- and 10-bit
    // semi-planar set -- 4:2:0 (NV12, P010) and 4:4:4 (NV24, S410) -- is
    // encodable as it stands and answers VK_TRUE always. Being DEVICE-BLIND,
    // that VK_TRUE says the library routes the format unconverted and does
    // not say this device has an encode profile at that subsampling; the
    // session refuses with the driver's reason when it does not. The rest of
    // the accepted set -- 12-bit semi-planar 4:2:0, the 3-plane 4:2:0 set,
    // the 8-bit UNORM RGBA family, and the packed 4:4:4 layouts AYUV and Y410
    // under a Y'CbCr declaration -- is encodable only after conversion, so
    // those answer VK_TRUE only when THIS SESSION was configured with that
    // exact input PAIR: VK_FALSE before InitializeExt(), and VK_FALSE on a
    // session declared in some other format, or in the same format under the
    // other colour model. That last case is not a corner -- AYUV and an
    // ordinary R'G'B' image share R8G8B8A8_UNORM, and a session built for one
    // converts nothing for the other.
    //
    // Consequence for callers: the ANSWER FOR A 3-PLANE FORMAT CHANGES ACROSS
    // InitializeExt(). Query it after initializing the session you intend to
    // submit to, not before.
    //
    // GetMaxWidth/GetMaxHeight report the PROBED device capability and return
    // 0 before InitializeExt(). Zero means "not yet known", not "unsupported":
    // pre-init there is nothing to report, and a fabricated bound is worse
    // than an admitted unknown because it cannot be distinguished from a
    // measurement.
    virtual VkBool32 SupportsFormat(VkFormat inputFormat) const = 0;
    virtual uint32_t GetMaxWidth() const = 0;
    virtual uint32_t GetMaxHeight() const = 0;

    // === Device Access ===

    // Get the encoder's Vulkan device handles.
    // Use these for DMA-BUF import, semaphore creation, etc.
    // The encoder owns these handles — caller must NOT destroy them.
    virtual VkDevice GetVkDevice() const = 0;
    virtual VkPhysicalDevice GetVkPhysicalDevice() const = 0;
    virtual VkInstance GetVkInstance() const = 0;

    // The loader entry point these handles were resolved through.
    //
    // WHY THIS EXISTS, and why the three getters above are not enough. An
    // embedder that owns no Vulkan device of its own still has to resolve
    // device-level functions to touch the handles above -- to fill a staging
    // image, to import a semaphore. Its own function-pointer table cannot
    // serve: on a host where the embedder never brought Vulkan up there is
    // nothing in it, and -- far worse -- on a host where the embedder
    // ATTEMPTED Vulkan and failed, the table can be left holding a
    // vkGetInstanceProcAddr that is non-null and DANGLING, pointing into a
    // loader mapping that was torn down when the attempt failed. A null
    // check passes and the call jumps into a non-executable page.
    //
    // So the rule is not "use ours when yours is missing", it is "on a
    // library-owned device, ours is the only entry point with a defined
    // lifetime": it stays valid exactly as long as the encoder does.
    //
    // Returns nullptr before the device context is brought up. The library
    // owns this; the caller must not unload the loader behind it.
    virtual PFN_vkGetInstanceProcAddr GetVkGetInstanceProcAddr() const = 0;

    // === Runtime Info ===

    // Populate *outInfo with the encoder's current runtime characteristics.
    // Valid only after InitializeExt() succeeds (returns VK_NOT_READY
    // otherwise; VK_ERROR_INITIALIZATION_FAILED if outInfo is null, if its
    // sType is not VK_VIDEO_ENCODER_STRUCTURE_TYPE_RUNTIME_INFO, or if
    // anything is chained onto its pNext). The rate-control MODE is fixed
    // for the session -- Reconfigure() refuses a change to it -- so
    // trustedRateController does not change once the session is up.
    virtual VkResult GetRuntimeInfo(VkVideoEncoderRuntimeInfo* outInfo) const = 0;

    // === Handle exchange ===

    // Import |descriptor| once and return an id naming the result. The import
    // and every allocation it needs happen here, not on the submit path.
    //
    // The caller owns the key -> id map. The library deliberately does NOT
    // register-if-absent, because that puts allocation back on submit.
    //
    // VK_IMAGE registrations are the same-device OPT-IN arm, with a hard
    // precondition: the image must remain valid, and its handle value
    // stable, for the LIFETIME of the registration, and the
    // caller must Unregister BEFORE destroying it -- a driver may recycle
    // the handle value, and a surviving registration would then name freed
    // memory. Per-frame ephemeral images must use an OS-handle registration
    // (DMA_BUF et al) instead. Supplying |imageUsage| for a VK_IMAGE
    // registration lets the library build spec-clean views and, when it
    // includes VIDEO_ENCODE_SRC, register the image as one the encoder can
    // read as it stands. Leaving it 0 is accepted, but the slot is then
    // presumed transfer-source only: access the caller never declared is
    // never granted, so nothing here can make the encode read an image
    // without encode usage.
    //
    // |osHandle| is an int fd on POSIX and a HANDLE on Windows, passed as
    // uint64. Ownership follows |descriptor.ownership| (TRANSFER unless the
    // caller says otherwise): under TRANSFER an fd is consumed on EVERY
    // exit path including failure; under BORROW the caller keeps its fd on
    // every exit path (the library works on an immediate private
    // duplicate). A Win32 handle is never closed by the library in either
    // mode.
    //
    // |pStatus|, optional and self-stamped, receives the ownership echo
    // (handlesConsumed) on every return -- assert against it rather than
    // guessing. A non-null |pStatus| with the wrong sType is version skew
    // and is refused (STRUCTURE_TYPE_UNKNOWN), with the ownership rule
    // still applied to the handle on that exit like every other.
    //
    // Chain a VkVideoEncoderImportGuardInfo onto |pStatus| to learn what
    // the dma-buf import-ordinal guard did for THIS registration -- on a
    // stock build, DISABLED, because the guard is disabled by default; on a
    // build that re-arms it, the workaround's own report, which a caller
    // with silenceStdio set can obtain no other way. It is written on
    // every return that gets past the structure-type gate, including the
    // failures. An unknown or repeated chained sType is the same version
    // skew as a mis-stamped pStatus and is refused the same way.
    //
    // Returns a typed status. VK_VIDEO_ENCODER_STATUS_ERROR_MODIFIER_UNSUPPORTED
    // is renegotiable; DEVICE_MISMATCH and USAGE_INSUFFICIENT are not.
    virtual VkVideoEncoderStatusCode RegisterImageResource(
        const VkVideoEncoderExternalImageDescriptor& descriptor,
        uint64_t osHandle,
        VkVideoEncoderResource* outResource,
        VkVideoEncoderStatus* pStatus = nullptr) = 0;

    // Retire a registration. Deferred and REFCOUNTED: the underlying image is
    // freed only once no submitted frame can still read it, and the call does
    // not stall on a device wait to establish that.
    //
    // The id is invalid immediately on return; any later use is rejected
    // rather than dereferenced.
    virtual VkVideoEncoderStatusCode UnregisterImageResource(
        VkVideoEncoderResource resource) = 0;

    // Answer whether a descriptor WOULD register, without importing
    // anything and without a handle -- so a producer can negotiate before it
    // allocates, rather than discovering the answer per frame, mid-stream,
    // as a driver error.
    //
    // It runs the SAME predicate RegisterImageResource runs, so the query and
    // the registration cannot disagree.
    //
    // |supported| VK_FALSE is never the end of the story: |status| names the
    // reason, and the renegotiable ones (MODIFIER_UNSUPPORTED,
    // CONVERSION_REQUIRED) are what let a producer pick different allocation
    // parameters instead of giving up.
    //
    // Handle types whose arms are reserved but not yet implemented answer
    // VK_FALSE with ERROR_HANDLE_TYPE_UNSUPPORTED, so a caller branches
    // cleanly rather than building a pipeline on a promise.
    virtual VkVideoEncoderStatusCode QueryImageSupport(
        const VkVideoEncoderExternalImageDescriptor& descriptor,
        VkVideoEncoderImageSupport* outSupport) = 0;

    // Import a synchronization primitive once and return an id naming it.
    //
    // |osHandle| follows the same ownership contract as image registration:
    // |descriptor.ownership| selects the mode (TRANSFER by default: an fd
    // is consumed on every exit path including failure; BORROW: the caller
    // keeps it on every exit path); a Win32 handle is never closed by the
    // library in either mode. |pStatus| is the same optional echo, with one
    // difference: it carries no chain here. See the note on
    // VkVideoEncoderStatus for why, and for what the refusal costs an fd
    // presented under TRANSFER.
    //
    // CROSS-PROCESS ON WINDOWS: |osHandle| must already be valid in the
    // encoder's process. A Win32 handle is process-local, and this interface
    // carries no source PID, so the caller performs the
    // OpenProcess(PROCESS_DUP_HANDLE) + DuplicateHandle itself and passes the
    // duplicate. It also closes that duplicate once registration returns --
    // the library never closes a handle it did not create.
    //
    // Duplicate on the IMPORTING side, not the exporting side. Pre-injecting
    // a duplicate from the exporter collides with the importer's handle
    // table.
    //
    // Timeline semaphores only -- see the descriptor for why.
    virtual VkVideoEncoderStatusCode RegisterSemaphore(
        const VkVideoEncoderSemaphoreDescriptor& descriptor,
        uint64_t osHandle,
        VkVideoEncoderResource* outResource,
        VkVideoEncoderStatus* pStatus = nullptr) = 0;

    // Retire a semaphore registration. Unlike an image, this does NOT
    // defer: the imported VkSemaphore is destroyed before the call returns.
    // The caller must therefore ensure that every submitted batch which
    // waits on or signals this registration has completed execution
    // (VUID-vkDestroySemaphore-semaphore-01137) before unregistering, and
    // must not name the id afterwards. Registrations never retired are
    // destroyed at session teardown, after the encoder's threads are
    // joined and its queue is idle.
    virtual VkVideoEncoderStatusCode UnregisterSemaphore(
        VkVideoEncoderResource resource) = 0;

    // Submit a frame against a registration. This call performs no image
    // import and allocates nothing for the input: the image, its memory, its
    // view and its wrapper were created once at registration, and registered
    // semaphores named by id are resolved rather than imported.
    //
    // A chained per-frame fence descriptor is the exception: an armed
    // acquireFenceFd and a requested release fd are per-frame Vulkan objects,
    // and both retire with the frame.
    //
    // The registration is reference-counted for the lifetime of the submitted
    // frame, so an Unregister racing an in-flight frame defers rather than
    // freeing memory the GPU is still reading.
    //
    // Returns VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_UNKNOWN for a stale or
    // never-registered id -- including one retired since it was minted, which
    // the generation counter catches instead of silently matching a recycled
    // slot.
    //
    // Threading: class (b), and the warning there applies: the CPU-side
    // pipeline runs inline on this call; capacity is an up-front
    // VK_VIDEO_ENCODER_STATUS_NOT_READY (drain completions and retry the
    // same call), never a blocking wait; completion is what happens
    // asynchronously.
    //
    // WAIT-COUNT BOUND, direct path only. A registration that routes DIRECT
    // assembles its waits into a fixed eight-entry array, so the frame's
    // wait count plus its acquire fence must be <= 8. Beyond that this call
    // returns VK_VIDEO_ENCODER_STATUS_ERROR_RESOURCE_LIMIT and encodes
    // nothing, rather than discarding the surplus waits.
    // Retrying unchanged cannot succeed: present fewer waits, or use a
    // registration that is not zero-copy, whose wait list is not bounded
    // here. The refusal consumes an acquireFenceFd like
    // every other exit.
    virtual VkVideoEncoderStatusCode SubmitRegisteredFrame(
        const VkVideoEncoderFrameSubmitInfo& info,
        VkSemaphore* pStagingCompleteSemaphore) = 0;
};

// Factory function for the extended encoder interface
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExt(
    VkSharedBaseObj<VulkanVideoEncoderExt>& vulkanVideoEncoder);


//=============================================================================
// Encoder capability enumeration BEFORE InitializeExt().
//
// A consumer typically enumerates the profiles it can offer at process
// startup -- before any encoder session exists -- so that it can advertise
// them to a capability query or a codec negotiation. There is no
// VulkanVideoEncoderExt instance at that point, so this capability query
// must be a free function that does NOT create a VkDevice or an encode
// session. The capability answer itself comes from
// vkGetPhysicalDeviceVideoCapabilitiesKHR and
// vkGetPhysicalDeviceVideoFormatPropertiesKHR; physical-device properties,
// device extensions and queue-family video properties are read alongside
// them, because which probes are issued at all depends on those.
//
// A media/gpu/vulkan capability enumerator is intended to become a thin
// wrapper over these functions: it probes a fixed candidate set of codecs and
// reads back min/max coded extent and rate-control modes, which are the
// scalars VkVideoEncoderCapabilities below carries. The accepted input
// formats are a LIST and are read from VkEncEnumerateInputFormats on a
// context, so that the capacity is the caller's rather than a constant this
// header has to pick. A caller holding no Vulkan handles builds an OWN-mode
// context with a zero gpuUUID, which is the same bring-up the ephemeral entry
// point performs for itself.
//=============================================================================
// Whether an advertised input format is the one the encoder wants.
//
// STATED, and not derivable from the entry's two formats: a format the
// encoder does not read can still name ITSELF as its encode format, so
// format == encodeFormat holds for some SUBOPTIMAL entries exactly as it does
// for every OPTIMAL one.
//
// A property of the FORMAT, under this codec, profile and device. Whether one
// particular IMAGE is taken as it lies is a property of that allocation --
// its tiling and its modifier -- and QueryImageSupport is what answers it,
// per allocation.
typedef enum VkVideoEncoderInputFormatOptimality {
    // The encoder's own input format here: a frame supplied in it is what the
    // encoder reads.
    VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL    = 0,
    // Not the format the encoder reads. Accepted, and coded correctly,
    // because the library converts it into encodeFormat first. That
    // conversion is what the entry costs over an OPTIMAL one, so prefer an
    // OPTIMAL entry wherever the producer can supply one.
    //
    // There is no knob here: which formats can be converted is what this list
    // reports. A build that can perform no conversion advertises no
    // SUBOPTIMAL entry at all, rather than advertising one and refusing the
    // session that declares it; its OPTIMAL entries are unaffected.
    VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL = 1,
} VkVideoEncoderInputFormatOptimality;

// One input format this encoder accepts: the format itself, the format the
// bitstream is coded from, and whether the encoder wants it.
//
// encodeFormat is what the encoder is given, so encodeFormat -- not format --
// is what decides the chroma subsampling and bit depth of the bitstream. A
// caller that hands over an RGBA frame reads from encodeFormat that the
// bitstream is coded from a Y'CbCr one.
//
// Read optimality for which entry to prefer and encodeFormat for what the
// bitstream carries; neither answers the other's question.
//
// Deliberately without sType/pNext. This is an array element the library
// WRITES, not a parameter structure a caller fills, and a pointer inside an
// array element is what stops that array being marshalled as one block.
struct VkVideoEncoderInputFormatProperties {
    VkFormat                            format;
    VkFormat                            encodeFormat;
    VkVideoEncoderInputFormatOptimality optimality;
};

// A Std syntax-flag bitmask, as reported by the driver for one
// (codec, profile) pair.
//
// Interpret an entry against the codec the query named:
// VkVideoEncodeH264StdFlagsKHR, VkVideoEncodeH265StdFlagsKHR or
// VkVideoEncodeAV1StdFlagsKHR. All three are VkFlags and a query names
// exactly one codec, so one list carries whichever applies rather than three
// lists of which two are always empty.
typedef VkFlags VkVideoEncoderStdFlags;

struct VkVideoEncoderCapabilities {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_CAPABILITIES;
    const void*                 pNext = nullptr;

    VkVideoCodecOperationFlagBitsKHR codec;

    // Level range. maxLevelIdc is the numeric value of the codec-specific
    // Std level the driver reports -- StdVideoH264LevelIdc,
    // StdVideoH265LevelIdc or StdVideoAV1Level; minLevelIdc is 0 when the
    // driver does not expose a floor (Vulkan has no min-level cap today).
    uint32_t  minLevelIdc;
    uint32_t  maxLevelIdc;

    // DPB / reference limits (VkVideoCapabilitiesKHR).
    uint32_t  maxDpbSlots;
    uint32_t  maxActiveReferencePictures;

    // Encode caps (VkVideoEncodeCapabilitiesKHR).
    uint32_t  maxQualityLevels;
    uint64_t  maxBitrate;

    // Picture access granularity (VkVideoCapabilitiesKHR::pictureAccessGranularity).
    uint32_t  pictureAccessGranularityWidth;
    uint32_t  pictureAccessGranularityHeight;

    VkVideoEncodeRateControlModeFlagsKHR supportedRateControlModes;
    VkVideoEncodeCapabilityFlagsKHR      flags;

    // Coded-extent range (VkVideoCapabilitiesKHR).
    VkExtent2D minCodedExtent;
    VkExtent2D maxCodedExtent;

    // The driver's Std syntax flags and the input formats this encoder
    // accepts are NOT here. Each is a LIST, and a list is answered by its own
    // two-call entry point -- VkEncEnumerateStdFlags and
    // VkEncEnumerateInputFormats -- so that the capacity is the caller's
    // rather than a constant this header has to pick, and the membership
    // rules are stated where the answer is given. What is left in this
    // structure is scalars only, which is what keeps it pointer-free,
    // trivially copyable and 1:1 IPC-shaped.

    // Optional-feature availability. supportsMaintenance1 and
    // supportsQuantizationMap are device-extension presence;
    // supportsIntraRefresh additionally requires the device to report at
    // least one intra-refresh mode. supportsResizeWithoutIdr is false: no
    // Vulkan capability expresses it, so a resize forces an IDR.
    bool  supportsQuantizationMap;
    bool  supportsIntraRefresh;
    bool  supportsMaintenance1;
    bool  supportsResizeWithoutIdr;   // for caller-choice IDR-on-resize
};

//=============================================================================
// Capability probing, per codec AND profile.
//
// vkGetPhysicalDeviceVideoCapabilitiesKHR answers per VkVideoProfileInfoKHR,
// so a caller advertising H.264 Baseline/Main or H.265 Main-10 must probe
// those exact (profile, bit-depth) combinations rather than copy a sibling
// profile's result. VK_VIDEO_ENCODER_PROFILE_DEFAULT probes the codec's
// representative profile -- H.264 High, H.265 Main, AV1 Main, all 8-bit --
// which is the whole of what a caller with no particular profile in mind
// needs to ask.
//
// Both return VK_SUCCESS and fill *outCaps on success, and
// VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR when `profile` is not
// one this library probes for `codec` -- including a profile number that
// belongs to a different codec. They differ in how they say "not this
// device". The caller-handles variant is GIVEN the device, so a codec op it
// does not probe is VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR and a
// device that does not support encode for `codec` is
// VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR. The ephemeral variant
// SELECTS the device first, so everything that leaves it with no candidate
// -- an unmatched deviceId, a device lacking the video extensions, or no
// device offering encode for `codec` -- is VK_ERROR_FEATURE_NOT_PRESENT.
// A caller treats any non-VK_SUCCESS as "do not advertise
// this profile" rather than as a hard error.
//=============================================================================

// The caller supplies the Vulkan handles. Use this when the caller has
// already initialized its VkInstance and VkPhysicalDevice. The library wraps
// them in an internal device context, probes WITHOUT creating a VkDevice or
// an encode session, and does not destroy the caller's handles.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult EnumerateVulkanVideoEncoderProfileCapabilities(
    VkInstance                       instance,        // caller-supplied
    VkPhysicalDevice                 physicalDevice,  // caller-supplied
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkVideoEncoderCapabilities*      outCaps);

// No caller-supplied handles. Use this from a caller that has not
// initialized Vulkan yet, and pass -1 for the library's default (first
// capable) device.
//
// "Ephemeral" names the API contract -- the caller supplies no handles and
// gets none back. It does NOT mean Vulkan is torn down between calls: an
// instance created here is held for the process lifetime, because re-creating
// one after a sandbox has locked down is a device loss.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult EnumerateVulkanVideoEncoderProfileCapabilitiesEphemeral(
    int32_t                          deviceId,        // -1 = first capable
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkVideoEncoderCapabilities*      outCaps);

//=============================================================================
// The encoder context
//
// A refcounted object holding a live VkInstance and the enumerated physical
// devices, so capabilities -- codecs, profiles, input formats, DRM modifiers,
// rate-control modes -- can be answered WITHOUT creating and destroying a
// loader, an instance or a device per query. A context creates no VkDevice;
// a session created on one creates its own.
//
// IMMUTABLE AFTER CONSTRUCTION. The device list, the identities and the
// per-(codec, profile) capability snapshot are all filled during construction
// and are read-only afterwards. That is what lets unrelated sequences SHARE
// ONE CONTEXT WITH NO SYNCHRONISATION OF THEIR OWN, and it is why there is no
// refresh entry point: a new generation of capability truth is a new context.
// TWO ACCESSORS READ THE DRIVER RATHER THAN THE SNAPSHOT, and both do so
// because their answer depends on something not known at construction. The
// DRM-modifier query is keyed on (format, usage). VkEncEnumerateInputFormats
// is keyed on (codec, profile) and resolves every candidate live through the
// same resolver the point query and InitializeExt use -- which is what makes
// the advertised set and the accepted set one set rather than two that can
// drift, and is therefore the reason it cannot be served from a snapshot
// probed at a fixed envelope. NEITHER TOUCHES CONTEXT STATE, so the
// immutability above is exactly what it says: the context is not written
// after construction, by these or by anything else.
//
// Lifetime rules, each from a specific hazard:
//
//  1. THE LOADER HANDLE IS NEVER UNLOADED. An embedder resolves its own
//     Vulkan entry points out of the same libvulkan.so.1 / vulkan-1.dll, and
//     unloading it when a context goes away would invalidate pointers the
//     embedder still holds. The context retains it for the process lifetime,
//     in BOTH modes, including for a context created and released inside a
//     single call.
//
//  2. A SUCCESSFULLY BUILT OWN-MODE CONTEXT IS NEVER DESTROYED. Re-issuing
//     vkCreateInstance after a sandbox has locked down is a device loss
//     rather than a slow path, so the library holds a floor reference to
//     every OWN-mode context it builds, keyed by the create-info's gpuUUID:
//     a second create returns the SAME context instead of standing up a
//     second instance. A create whose bring-up FAILS is not registered, so a
//     later create with the same key builds again. Callers layer their own
//     references on top and may drop them freely.
//
//  3. ADOPT MODE NEVER DESTROYS. The VkInstance and VkPhysicalDevice belong to
//     the embedder; release is a no-op on both. ADOPT contexts are
//     deliberately NOT floor-referenced: a cached borrowed handle would
//     outlive the embedder's ownership of it, and Vulkan handle values are
//     recycled, so a stale entry could match a different object.
//
//  4. Context identity is the (VkInstance, VkPhysicalDevice, VkDevice) triple.
//     A context creates no VkDevice, so the third element is VK_NULL_HANDLE
//     for every context this API builds.
//
// A host that already has a VkInstance may use either mode. A host with no
// Vulkan implementation of its own must use OWN.
//=============================================================================

// Opaque, refcounted, immutable after construction. Never defined here: the
// only operations on it are the free functions below.
class VulkanVideoEncoderContext;

typedef enum VkVideoEncoderContextMode {
    // Create and own a VkInstance, then enumerate physical devices.
    VK_VIDEO_ENCODER_CONTEXT_MODE_OWN    = 0,
    // Borrow an instance and physical device the embedder already has.
    // Nothing is created and nothing is destroyed on release.
    VK_VIDEO_ENCODER_CONTEXT_MODE_ADOPT  = 1,
} VkVideoEncoderContextMode;

struct VkVideoEncoderContextCreateInfo {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONTEXT_CREATE_INFO;
    const void*                 pNext = nullptr;

    // Deliberately WITHOUT a default member initializer. OWN is 0, so a
    // zero-filled create-info means OWN; giving this field a different
    // default would make `T x = {};` and `memset` disagree about what the
    // caller asked for. Set it explicitly.
    VkVideoEncoderContextMode   mode;

    // ADOPT only, and both are required TOGETHER -- either one alone is
    // VK_ERROR_INITIALIZATION_FAILED. Supplying either in OWN mode is also an
    // error: handles handed to a mode that would not borrow them mean the
    // caller asked for one thing and meant another.
    VkInstance                  adoptInstance = VK_NULL_HANDLE;
    VkPhysicalDevice            adoptPhysicalDevice = VK_NULL_HANDLE;

    // OWN only. An all-zero UUID means "enumerate everything"; a non-zero one
    // pins the context to that single physical device. deviceID cannot do
    // this job -- it is a PCI device id, so it cannot disambiguate two
    // identical GPUs. A non-zero UUID in ADOPT mode is an error: the device
    // is already chosen.
    uint8_t                     gpuUUID[VK_UUID_SIZE];

    // Latches the library's process-wide stdio-silence gate when VK_TRUE.
    // VK_FALSE does NOT un-silence: a capability probe must not undo a
    // session's choice, which is why this is not the unconditional set that
    // InitializeExt does with the same-named config field.
    VkBool32                    silenceStdio = VK_FALSE;
};

// Stable identity across processes and runs.
struct VkVideoEncoderDeviceIdentity {
    VkVideoEncoderStructureType sType =
        VK_VIDEO_ENCODER_STRUCTURE_TYPE_DEVICE_IDENTITY;
    const void*                 pNext = nullptr;

    uint8_t  deviceUUID[VK_UUID_SIZE];
    uint8_t  driverUUID[VK_UUID_SIZE];
    char     deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t vendorID;
    // VkPhysicalDeviceProperties::deviceID. Carried because the library's own
    // ephemeral capability entry points select on it, NOT because it is an
    // identity: two identical GPUs share one deviceID. Use deviceUUID to
    // identify a device and this only to reproduce a deviceID-based selection.
    uint32_t deviceID;
};

// Build a context. See the lifetime rules above: in OWN mode this may return
// an existing context rather than a new one, and the returned context is
// never destroyed. In ADOPT mode a fresh context is built every time and
// destroying it destroys nothing of the caller's.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderContext(
    const VkVideoEncoderContextCreateInfo*      pCreateInfo,
    VkSharedBaseObj<VulkanVideoEncoderContext>& outContext);

// Create an encode session ON a context. This is the session-to-context
// link, and it is how a session is meant to obtain a borrowed instance and
// physical device.
//
// The session creates its own VkDevice on the context's |deviceIndex|
// physical device -- a context creates none, in either mode -- so two
// sessions on one context are two devices on one instance, not a shared
// device.
//
// The session holds a reference to |context| for its whole life. That keeps
// the CONTEXT OBJECT -- and the capability snapshot the caller selected from --
// alive and addressable while the session runs.
//
// IT DOES NOT KEEP THE BORROWED VkInstance ALIVE, and no reference here could.
// In ADOPT the instance belongs to the embedder and the context destroys
// nothing on release (lifetime rule 3 above); in OWN the library's floor
// registry already holds the context above zero for the process lifetime
// (rule 2). AN EMBEDDER THAT DESTROYS ITS VkInstance WHILE A SESSION IS LIVE
// HAS A USE-AFTER-FREE, on this path exactly as on the config path. That
// hazard is the embedder's to avoid.
//
// THE SESSION DOES NOT CONSUME THE CONTEXT'S CAPABILITY SNAPSHOT, which is
// the assumption to avoid making. It reads the instance and the physical
// device out of the context and nothing else, then re-probes queue families,
// device extensions and encode capabilities for itself, so N sessions on one
// context run N+1 capability sweeps rather than one.
//
// A CONFIG THAT ALSO SELECTS A DEVICE IS REFUSED with
// VK_ERROR_INITIALIZATION_FAILED, not resolved: externalInstance,
// externalPhysicalDevice, a deviceId other than -1, and a non-zero gpuUUID all
// name a device the context has already chosen. (The config ADOPT path
// answers VK_ERROR_FEATURE_NOT_PRESENT for the same user error; two codes for
// "you named the device twice", and this path's is the typed one.) A stale
// config field outranking the context would send the session to a different
// GPU than the one whose capabilities the caller just read, which on a
// single-GPU host is entirely invisible.
//
// config.externalDevice is refused for a different reason: a context owns
// the instance and the physical device and creates no VkDevice (context
// rule 4), and a session built on one creates its own -- so a caller-
// supplied logical device has nowhere to land on this path.
//
// |deviceIndex| indexes the context's snapshot -- 0..VkEncGetPhysicalDeviceCount-1,
// and is always 0 in ADOPT mode. Out of range is VK_ERROR_INITIALIZATION_FAILED.
//
// ON FAILURE |vulkanVideoEncoder| IS LEFT UNTOUCHED, matching
// CreateVulkanVideoEncoderExt. It is not nulled: a failing create must not
// destroy a session the caller already had in that variable.
//
// InitializeExt() is still what starts the session. This decides only where
// its instance and physical device come from.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult CreateVulkanVideoEncoderExtOnContext(
    const VkSharedBaseObj<VulkanVideoEncoderContext>& context,
    uint32_t                                          deviceIndex,
    VkSharedBaseObj<VulkanVideoEncoderExt>&           vulkanVideoEncoder);

// Number of physical devices in the context's snapshot. Always 1 in ADOPT
// mode. 0 for a null context -- the count query has no error channel, and a
// caller that then indexes 0..count-1 does nothing at all, which is the
// correct behaviour for a context that was never built.
extern "C" VK_VIDEO_ENCODER_EXPORT
uint32_t VkEncGetPhysicalDeviceCount(VulkanVideoEncoderContext* ctx);

extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncGetPhysicalDeviceIdentity(
    VulkanVideoEncoderContext*      ctx,
    uint32_t                        deviceIndex,
    VkVideoEncoderDeviceIdentity*   pOut);

// Read one (codec, profile) entry out of the snapshot. No driver call is
// made: the answer was computed in the constructor. Returns exactly what the
// probe returned at construction time, so a caller maps any non-VK_SUCCESS to
// "do not advertise this profile" the same way it does for the free-function
// enumerators. *pOut is written only when the entry was actually probed.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncGetEncodeCapabilities(
    VulkanVideoEncoderContext*       ctx,
    uint32_t                         deviceIndex,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    VkVideoEncoderCapabilities*      pOut);

// The Std syntax flags the driver reports for (|codec|, |profile|) on
// |deviceIndex|.
//
// Two-call idiom: pFlags == nullptr writes the number of entries to *pCount;
// otherwise at most *pCount entries are written, *pCount is set to the number
// written, and VK_INCOMPLETE is returned if any entry was dropped.
//
// *pCount IS ALWAYS WRITTEN, and this rule is shared by all three list
// enumerators -- this one, VkEncEnumerateInputFormats and
// VkEncEnumerateDrmModifiers. A null pCount is the single exception, being
// the one failure that leaves nowhere to write to; on every other return,
// success or failure, the caller is left a defined count: the number of
// entries written on VK_SUCCESS and VK_INCOMPLETE, and 0 on every error.
// One wrapper can therefore cover all three and read *pCount without
// first having to ask which of them it called.
//
// The count is a property of the context's snapshot, which is immutable after
// construction, so the counting call and the fetching call cannot disagree.
// VK_INCOMPLETE here means the caller passed a smaller *pCount than it was
// told, and never that the answer moved underneath it.
//
// A (codec, profile) pair this library does not probe, or one the driver
// refused, propagates the same result VkEncGetEncodeCapabilities gives for
// that pair and writes a count of 0.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateStdFlags(
    VulkanVideoEncoderContext*       ctx,
    uint32_t                         deviceIndex,
    VkVideoCodecOperationFlagBitsKHR codec,
    uint32_t                         profile,
    uint32_t*                        pCount,
    VkVideoEncoderStdFlags*          pFlags);

// The input formats this encoder accepts for (|codec|, |profile|) on
// |deviceIndex|, what each one is encoded as, and how it gets there.
//
// MEMBERSHIP. An entry is present when this library can encode that format on
// THIS device for THIS (codec, profile). Two rules, and they are the whole
// contract:
//
//   * a format the encoder itself reads is advertised as
//     VK_VIDEO_ENCODER_INPUT_FORMAT_OPTIMAL, with encodeFormat == format;
//   * a format the encoder does not read, but that the preprocess compute
//     filter converts into one it does, is advertised as
//     VK_VIDEO_ENCODER_INPUT_FORMAT_SUBOPTIMAL, with encodeFormat naming the
//     conversion's output and so what its bitstream is coded from -- and only
//     when the device accepts that output as an encode input. A conversion
//     whose result this device would not take is not a route, so it is not
//     advertised.
//
// WHAT THE LIST IS FOR. It is the set of formats a caller may DECLARE as a
// session's input format and then hand in -- the WHOLE set, and not a
// preference inside a wider one. InitializeExt gates acceptance on this same
// answer, so for this (codec, profile) a format on the list initialises and a
// format the list omits is refused, with the format and its subsampling named.
// The one axis on which absence is not a refusal is the colour model, which
// this list cannot carry: see the paragraph on the packed 4:4:4 layouts below,
// and VkEncQueryInputFormatSupport, which takes one.
//
// A session is narrower than the profile: an OPTIMAL entry is taken by any
// session on this profile, but a SUBOPTIMAL entry is taken only by a session
// that DECLARED that format, because the conversion is built for the one input
// format the session was given. Sizing a producer pool from this list is safe
// precisely because the declaration comes first.
//
// AND A SUBOPTIMAL ENTRY IS TAKEN ONLY ON THE REGISTERED LANE. The conversion
// runs against a REGISTERED resource, so a SUBOPTIMAL format must be handed in
// through RegisterImageResource() followed by SubmitRegisteredFrame().
// SubmitExternalFrame() refuses it with VK_ERROR_FORMAT_NOT_SUPPORTED: that
// path has no registration to attach a conversion to. An OPTIMAL entry is
// accepted on either lane. This is a qualification of the sentence above, not
// an exception to it -- the format is still one the session may declare; what
// is restricted is which submit carries it.
//
// The list carries no tiling and no modifier: tiling is a property of an
// image rather than of a format, and QueryImageSupport is what answers it for
// a specific allocation. It carries no colour model either -- the packed
// 4:4:4 Y'CbCr layouts have no Vulkan enumerant and ride the RGBA ones -- so
// an aliased enumerant that IS advertised appears here under its RGB reading,
// and its Y'CbCr reading is reached only by declaring it. An enumerant whose
// only accepted reading is the Y'CbCr one does not appear in this list at
// all, so ON THE COLOUR-MODEL AXIS absence from it is not a refusal. That is
// the single exception to "the list is what InitializeExt takes", it exists
// because a list of formats cannot express a declaration about samples, and
// VkEncQueryInputFormatSupport is the surface that closes it.
//
// ORDER AND MULTIPLICITY. Each format appears once, however many tilings the
// device reports it at; the OPTIMAL entries come first and the SUBOPTIMAL ones
// follow, each group in this library's own routable order.
//
// NOT IN THE DEVICE'S ORDER, and there is no longer such an order to give.
// Every candidate is resolved at the profile ITS OWN binding derives -- a
// 4:4:4 input at a 4:4:4 profile, a 10-bit one at a 10-bit profile -- so the
// entries of one list are answered against as many device queries as there are
// distinct derivations, and no single device list orders them. Do not read
// position as preference beyond the OPTIMAL/SUBOPTIMAL split, which is stated
// per entry and is the part that means something.
//
// Two-call idiom and the *pCount rule exactly as VkEncEnumerateStdFlags
// states them: pFormats == nullptr writes the number of entries to *pCount;
// otherwise at most *pCount entries are written, *pCount is set to the number
// written, and VK_INCOMPLETE is returned if any entry was dropped. A pair
// this library does not probe, or one the driver refused, propagates the
// result VkEncGetEncodeCapabilities gives for it and writes a count of 0.
//
// THE STABILITY RULE IS THE SAME AND ITS REASON IS NOT. The counting call and
// the fetching call cannot disagree, so VK_INCOMPLETE here means the caller
// passed a smaller *pCount than it was told and never that the answer moved
// underneath it -- but this list is NOT read out of the context's snapshot.
// It is recomputed live on every call, per candidate, against the device.
// What makes the two calls agree is that the resolve is a deterministic
// function of (context, device, codec, profile) and a context's devices do
// not change for its lifetime, which is the same premise the snapshot itself
// rests on rather than the snapshot itself.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateInputFormats(
    VulkanVideoEncoderContext*           ctx,
    uint32_t                             deviceIndex,
    VkVideoCodecOperationFlagBitsKHR     codec,
    uint32_t                             profile,
    uint32_t*                            pCount,
    VkVideoEncoderInputFormatProperties* pFormats);

// DRM modifiers for |format| that carry the format features |usage| implies.
// Answerable WITHOUT a session -- required, because the producer picks a
// modifier at allocation time, long before any encoder exists.
//
// Two-call idiom: pModifiers == nullptr writes the matching count to *pCount;
// otherwise at most *pCount entries are written, *pCount is set to the number
// written, and VK_INCOMPLETE is returned if any match was dropped. *pCount is
// written on every return but a null pCount, as VkEncEnumerateStdFlags
// states for all three enumerators -- so both error returns described below
// leave a count of 0, and so does an unrecognised deviceIndex.
//
// usage == 0 means "no feature filter" and returns every modifier the device
// reports for the format. A usage bit this function has no format-feature
// mapping for is VK_ERROR_INITIALIZATION_FAILED, never a silently dropped
// term: the whole point of the filter is that the caller can trust it.
//
// THIS ANSWER IS PROFILE-BLIND, AND THAT IS A LIMIT ON WHAT IT MEANS. It comes
// from vkGetPhysicalDeviceFormatProperties2, whose query has no video-profile
// term at all -- unlike VkEncEnumerateInputFormats and
// VkEncQueryInputFormatSupport, which are keyed on (codec, profile). So a
// modifier reported here as carrying VIDEO_ENCODE_INPUT is NOT thereby usable
// for the profile the session will negotiate: it says the FORMAT supports the
// feature under that modifier, not that this codec and profile accept that
// format. Ask the input-format query about the (codec, profile, format) triple
// and this one about the modifier, and treat the two answers as conjoined.
//
// Returns VK_ERROR_EXTENSION_NOT_PRESENT when the device cannot answer in
// 64-bit format features (VK_KHR_format_feature_flags2 / Vulkan 1.3). The
// 32-bit modifier list cannot express VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT,
// which is the one feature this entry point exists to filter on, so an
// answer from it would be wrong rather than partial. A device with no
// VK_EXT_image_drm_format_modifier at all reports a count of 0 and
// VK_SUCCESS: no modifiers is a true answer, not a failure.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncEnumerateDrmModifiers(
    VulkanVideoEncoderContext* ctx,
    uint32_t                   deviceIndex,
    VkFormat                   format,
    VkImageUsageFlags          usage,
    uint32_t*                  pCount,
    uint64_t*                  pModifiers);


// Whether this library will take |format|, declared in |colorModel|, as the
// input of a session on (|codec|, |profile|) on |deviceIndex| -- and what the
// bitstream would then be coded from.
//
// A POINT QUESTION, ASKED BEFORE A SESSION EXISTS, which is what separates it
// from everything else here. QueryImageSupport answers for a whole image
// descriptor, but it is a session method: it cannot be reached until a session
// has already been built in the very pair being asked about. This is the
// question a producer has EARLIER than that -- before it allocates a frame
// pool, while changing its mind is still free.
//
// NOT AN ENUMERATOR. No pCount, no two-call idiom, no VK_INCOMPLETE. It
// answers the one pair the caller names and enumerates nothing, and that is
// precisely why it can carry a colour model at all: a declaration is the
// caller's statement about its own samples, so a LIST carrying one would be a
// cross-product of formats and declarations rather than a list of formats.
//
// WHY THE COLOUR MODEL IS A PARAMETER HERE AND AN OMISSION THERE.
// VkEncEnumerateInputFormats says of itself that an enumerant whose only
// accepted reading is the Y'CbCr one "does not appear in this list at all, so
// absence from it is not a refusal". The packed 4:4:4 layouts AYUV and Y410
// have no Vulkan enumerant of their own and ride the RGBA ones, so that list
// can show neither of them as itself. This is where a caller holding AYUV or
// Y410 frames finds out.
//
// IT ANSWERS WHAT InitializeExt WILL ANSWER, AND BY THE SAME ROUTE, IN BOTH
// HALVES. The library half of the verdict is produced by running the binder
// InitializeExt runs, not by restating its rules -- so a pair reported here as
// supported is a pair that binds, and the profile rules (which profile admits
// which bit depth and which chroma subsampling) are applied in one place only.
// The device half is a live format query against |deviceIndex|, at the chroma
// subsampling and bit depth the input itself implies, and it is the SAME call
// InitializeExt makes before it creates a session. A pair refused here is
// refused there, with the format and its subsampling named.
//
// ONE RESOLVER, THREE SURFACES. VkEncEnumerateInputFormats answers the same
// question for the whole routable set at once, by running this function over
// every candidate; InitializeExt asks it about the one pair a session
// declared. So no two of them can disagree: a format on the advertised list is
// a format this entry point accepts and a format a session initialises with,
// naming the same encodeFormat and the same optimality. Use the list to
// discover, this to ask about one pair -- including one the list cannot carry,
// because the list has no colour-model argument and this does -- and expect
// initialisation to say exactly what they said.
//
// |profile| is the codec standard's own number, and
// VK_VIDEO_ENCODER_PROFILE_DEFAULT means "derive it from the input" exactly as
// VkVideoEncoderConfig::profile does. DEFAULT is the interesting value for a
// 4:4:4 input, because the derivation is what reaches a 4:4:4 profile: a
// profile number this library does not bind is refused here as it is at
// InitializeExt, so naming one is not a way round that.
//
// RETURNS
//   VK_SUCCESS
//       accepted, and this device encodes it. *pProperties is written.
//   VK_ERROR_FORMAT_NOT_SUPPORTED
//       the library will not take the pair on this (codec, profile), or this
//       device does not encode what the pair would be coded from.
//   VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR
//       |codec| is not an encode codec this library carries.
//   VK_ERROR_INITIALIZATION_FAILED
//       |ctx| is null, or |deviceIndex| names no device.
//
// |pProperties| may be NULL when only the verdict is wanted. It is written
// ONLY on VK_SUCCESS, and never partially.
//
// A REFUSAL EXPLAINS ITSELF ON THE LIBRARY'S STDERR, as InitializeExt's does
// and for the same reason -- it is the same binder. A caller sweeping formats
// to size a pool should expect that output; silenceStdio is a session-level
// control and does not reach this call.
extern "C" VK_VIDEO_ENCODER_EXPORT
VkResult VkEncQueryInputFormatSupport(
    VulkanVideoEncoderContext*           ctx,
    uint32_t                             deviceIndex,
    VkVideoCodecOperationFlagBitsKHR     codec,
    uint32_t                             profile,
    VkFormat                             format,
    VkVideoEncoderColorModel             colorModel,
    VkVideoEncoderInputFormatProperties* pProperties);


#endif /* _VULKAN_VIDEO_ENCODER_EXT_H_ */
