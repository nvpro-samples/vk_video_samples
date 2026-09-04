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

/*
 * INPUT-FORMAT *ENCODE* MATRIX, on the LIBRARY-OWNED device.
 *
 * WHY THIS EXISTS BESIDE encoder-ext-format-matrix. That test walks the same
 * nine formats and proves REGISTRATION and ROUTING -- a descriptor is
 * accepted and a slot resolves to DIRECT/FILTER/STAGED. It contains no
 * encode call at all: zero submits, zero bitstreams. "Registers and routes
 * FILTER" is a strictly weaker claim than "encodes", and the gap between
 * them is exactly where a filter that dispatches nothing, or converts pure
 * black, has hidden on this project before (see the filter suite that
 * reported 54/54 while converting black).
 *
 * WHAT THIS ADDS. For every format the device can carry a profile for, it
 *   1. fills a real image with a KNOWN four-quadrant primaries pattern,
 *      written in that format's own layout (RGB direct for the RGBA arm;
 *      BT.709 limited-range YCbCr for the YCbCr arms, so one comparison
 *      harness serves both);
 *   2. submits frames through SubmitRegisteredFrame;
 *   3. drains the bitstream out of the library and writes it to a file for
 *      an INDEPENDENT decoder (ffmpeg, outside this process) to judge.
 *
 * WHAT IT CAN FAIL ON -- stated up front:
 *   - a format whose session initializes, registers and routes correctly can
 *     still produce ZERO bytes of bitstream. That is the whole point: it is
 *     the failure the routing test cannot see.
 *   - the filter observable is read from GetCompletionInfo's chained
 *     VkVideoEncoderFilterInfo and reported as a PAIR (dispatch, staged)
 *     against the frame count. A dispatch count that is non-zero but does
 *     not track frames, or that rises while stagedCopyCount also rises, is
 *     visible here rather than averaged away.
 *   - bytes alone prove nothing about colour. Colour is judged by an
 *     INDEPENDENT DECODER -- ffmpeg, a separate process reading the written
 *     file -- and this program never decodes its own bitstream in-process.
 *     What it does do, since the assertion was wired up, is RUN that decoder
 *     and grade the four quadrant centres it returns; the promise used to
 *     stop at "is judged out of process" with nothing anywhere that judged.
 *
 * SAFETY -- multi-planar staging is a GPU HANG on this hardware
 * (VK_ERROR_DEVICE_LOST, 0-byte bitstream). A 3-plane row whose
 * slot does not resolve to FILTER is ABANDONED BEFORE ANY SUBMIT rather than
 * encoded, and says so. The guard is unconditional and is not a diagnostic.
 *
 * THE BAR, IN FULL. Three numbers. The second was missing when this file was
 * written; the third was missing until the decode assertion below was wired
 * into the default verdict chain, and it is the only one of the three that
 * can see the compute filter stop executing:
 *
 *   sessions=8 encoded=8 abandoned=0 failures=0   (default and --declare-tso)
 *   decodeGated=8 decodeFailed=0                  (default and --declare-tso)
 *   0 "The Vulkan spec states" under --validate   (default and --declare-tso)
 *
 * That is the bar, at exit 0.
 * decodeGated is 8 and not 9 ON PURPOSE: the HDR10 row's colour volume is
 * BT.2020/PQ, so the BT.709 quadrant comparison would fail on a CORRECT
 * encode. It is judged by CheckHdrSignalling() instead and counted
 * separately, because folding two different assertions into one number is how
 * "eight of nine rows were judged" would read as green.
 *
 * THE THIRD LINE IS NOT REDUNDANT WITH THE FIRST. Delete the four
 * m_vkDevCtx->CmdDispatch calls in VulkanFilterYuvCompute.cpp and the first
 * line is UNCHANGED -- sessions=8 encoded=8 abandoned=0 failures=0, exit 0,
 * dispatch=12 on every FILTER row -- while all five filter-routed rows decode
 * to a flat (0,76,0) in every quadrant. See kQuadTolerance below for the
 * measurement and for why the threshold is what it is.
 *
 * The validation half is stated because leaving it out cost this suite seven
 * real messages. `--validate` emitted 7 x
 * VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820 -- one per session --
 * while the counter half read green, because a counter that only counts
 * bitstreams cannot see a barrier. They were not the library's: they came
 * from this file's own producer emulation in UploadPattern, whose handover
 * barrier named VK_ACCESS_SHADER_READ_BIT on the ENCODE family. Fixed there;
 * see the comment on toGen.dstAccessMask.
 *
 * --foreign-residency is EXCLUDED from the bar and exits non-zero by design:
 * its two DIRECT rows drive a driver defect (see g_foreignResidency).
 *
 * AV1, ADDED FOR THE BLACKWELL SESSION -- AND THE NUMBERS ABOVE ARE FOR THE
 * REFERENCE HOST, WHICH CANNOT RUN IT.
 *
 * Four AV1 rows now sit at the bottom of kRows. On an RTX A4000 (GA104) there
 * is no VK_KHR_video_encode_av1 at all, so all four report DEVICE-LIMITED and
 * contribute nothing but a deviceLimited count. The full summary line, as
 * The full summary line on such a device is:
 *
 *   sessions=8 encoded=8 abandoned=0 failures=0
 *   decodeGated=8 decodeFailed=0 deviceLimited=6 av1Unverified=0
 *   hdrGated=1 hdrFailed=0
 *
 * The eighth session is the HEVC Main 8-bit row and the ninth is the HDR10
 * row.
 *
 * THE EIGHTH SESSION IS THE HEVC MAIN 8-BIT ROW AND ITS NUMBER IS PREDICTED,
 * NOT MEASURED -- see the comment on that row in kRows. Every other number in
 * this block was measured on an RTX A4000 at driver 615.06; that host is now
 * on 620.18, so the whole block is due a re-measure and a difference is not
 * automatically ours. Treat 8 as the expectation to confirm, not a result.
 *
 * deviceLimited counts P012 and I420-12 (no 12-bit profile on this device)
 * plus the AV1 rows. On hardware that HAS AV1 encode those rows join the
 * encoded set -- and the "12/12/12" this line used to predict was simply
 * wrong arithmetic (9 + 4 = 13), quite apart from the two AV1 HDR rows added
 * since. On a device that HAS AV1 encode the summary line reads:
 *
 *   sessions=15 encoded=15 abandoned=0 failures=0
 *   decodeGated=12 decodeFailed=0 deviceLimited=2 av1Unverified=0
 *   hdrGated=3 hdrFailed=0
 *
 * deviceLimited falls from 6 to 2 there: the AV1 rows run, and only the two
 * 12-bit rows remain refused. decodeGated stays at 12 and hdrGated rises to 3
 * because the three HDR rows -- one H.265 and two AV1 -- take
 * CheckHdrSignalling() rather than the quadrant gate, for the reason given at
 * the HDR row in kRows.
 *
 * "DEVICE-LIMITED" IS NOT TAKEN ON TRUST, which is the part that matters.
 * A no-session row costs nothing and fails nothing, so a new arm added this
 * way is a test that cannot fail by construction. Every AV1 row that gets no
 * session is therefore checked against the DEVICE -- see
 * ProbeAv1EncodeSupport() -- and a device that advertises the extension while
 * the session refuses to start is a FAILURE, loudly, not a skip.
 *
 * WHAT THE AV1 ROWS DO NOT COVER, stated here so nobody reads their green as
 * broader than it is: this suite sets disableFileOutput, which takes
 * VkVideoEncoderAV1's IN-MEMORY capture arm. The DKIF/'AV01' IVF muxer --
 * BuildFrameObuSequence and FlushBatchedTemporalUnit -- lives on the FILE arm
 * and is not reached from here, nor from Chromium. See the AV1 macro below.
 */

#include "vulkan_video_encoder_ext.h"
#include "vulkan_video_encoder_ext_internal.h"

#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_av1std.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;
const uint32_t kFrames = 12;

int g_failures = 0;

// DIAGNOSTIC ONLY. The X6/X4 formats put the sample in the HIGH bits of each
// 16-bit word (that is what the suffix means, and P010 -- same convention,
// same writer -- decodes correctly through the DIRECT path). This flag writes
// the sample in the LOW bits instead. It exists to tell a library-side
// misread apart from a writer-side convention error on the 3-plane 10-bit
// arm. It is not a fix and must not become one.
//
// This flag is a DISCRIMINATOR and not a description of a live bug: the row
// it names decodes out-of-process indistinguishably from the other rows.
//
// WHAT IS STILL TRUE, AND IS WHY THIS ROW'S GREEN IS NOT EVIDENCE FOR
// CHROMIUM: this harness's writer is MSB-aligned by design, and Chromium's
// PIXEL_FORMAT_YUV420P10 is LSB-aligned. The shape Chromium would produce for
// this row is not what this row exercises, which is exactly why
// VulkanVideoEncoderConfigBuilder::MapPixelFormat still answers
// VK_FORMAT_UNDEFINED for it.
bool g_rawBits = false;

// --declare-tso. THE POINT OF THIS MODE, stated where it is defined.
//
// The filter arm's LOCAL handback -- RestoreStagedInputLayout called from the
// filter branch of StageInputFrame -- does nothing for a consumer that
// declares GENERAL: GENERAL equals that arm's residual, so the helper's
// equal-layout early return fires and no barrier is recorded.
//
// This mode is the shape that exercises it. Declaring TRANSFER_SRC_OPTIMAL
// instead of GENERAL makes targetLayout differ from the filter arm's residual
// (GENERAL), so the early return does NOT fire and a real
// (GENERAL -> TRANSFER_SRC_OPTIMAL) barrier is recorded on every frame.
//
// It is not a contrived declaration: TRANSFER_SRC_OPTIMAL is the ext layer's
// own legacy-wrap default, and it is what a caller that pools a staging image
// and last used it as a copy source must state to be truthful.
//
// The loop closes with no missing arm: frame 1 acquires TRANSFER_SRC_OPTIMAL
// -> GENERAL (an arm that already exists), the handback returns the image to
// TRANSFER_SRC_OPTIMAL and RECORDS that, and frame 2 reads the record and
// names TRANSFER_SRC_OPTIMAL again.
bool g_declareTso = false;

// --content-probe. THE ONLY PLACE IN THE TREE THAT EXECUTES THE CONTENT
// PROBE'S CAPTURE.
//
// Every other assertion about the probe -- the whole of
// test/encoder-ext-import-content -- is device-free: it drives the scorer and
// the latch directly, and drives RegisterImageResource on a NULL-BACKEND
// session where no frame is ever submitted. So it can assert that a
// registration ARMS, and it cannot assert that an armed registration ever
// produces a VERDICT.
//
// That gap sat exactly on top of the change that needed it most. Arming a
// DIRECT (block-linear + VIDEO_ENCODE_SRC) registration is only useful
// because SetExternalInputFrameWithNode sends its first frame down a staged
// detour so the capture site in StageInputFrame can read it. If that detour
// silently stopped firing, the device-free suite would stay green and the
// registration would sit ARMED forever reporting NOT_EVALUATED -- an
// observable that cannot fail, which is the shape this suite's own C1c case
// declares unacceptable.
//
// THE ROW IS ALREADY THE RIGHT SHAPE, which is why this is a mode and not a
// new binary: DeclFor(ARM_DIRECT) declares VK_IMAGE_TILING_OPTIMAL plus
// VIDEO_ENCODE_SRC plus TRANSFER_SRC -- block-linear and directly encodable,
// i.e. the class the periodicity harness measured poisoned and the class the
// probe can be blind to -- and UploadPattern fills it with a four-quadrant
// colour bar before any frame is submitted. A colour bar is emphatically not
// a dead plane, so CLEAN is the answer a working capture must produce, and
// the arithmetic behind it is printed so the verdict is readable rather than
// merely asserted.
//
// WHAT EACH OUTCOME MEANS on a DIRECT row under this flag:
//   echo ARMED + final CLEAN + probed>=1 + armed==0  -> the detour ran, the
//        capture was recorded, the fence was waited and the bytes were scored.
//        This is the pass.
//   echo ARMED + final ARMED + probed==0 + armed==1  -> the arm decision works
//        and NOTHING DOWNSTREAM OF IT DOES. This is precisely the regression
//        the device-free suite cannot see, and it is a FAIL here.
//   echo NOT_APPLICABLE                              -> the arm predicate
//        regressed to reading encodeCapable/tiling again. FAIL.
bool g_contentProbe = false;

// --validate. THIS SUITE HAD NO WAY TO ENABLE THE VALIDATION LAYER AT ALL.
// Every sibling suite has one; this one did not, so setting VK_LAYER_PATH
// around it did exactly nothing and any "zero validation errors" reading
// taken from it was vacuous -- the layer was never in the instance. Found
// while trying to use this suite as a validation gate for the filter-arm
// restore; the first mutation run came back clean and the clean run was the
// bug.
bool g_validate = false;

// --foreign-residency. THE TWO RELEASE SITES THAT HAVE NEVER BEEN DRIVEN.
//
// VkVideoEncoder::ReleaseImageToForeignQueue is called from three places and
// only ONE of them had ever executed in any run in this tree: the staging COPY
// arm (StageInputFrame, !useComputeFilter), driven by
// encoder-ext-input-residency --foreign-opaque-fd. The other two -- the staging
// FILTER arm (StageInputFrame, compute-filter else-branch) and Path A
// (RecordVideoCodingCmd, after CmdEndVideoCodingKHR) -- had no traffic at all:
// grepping every log in the tree and on the GPU host for the [QFOT-REL] record
// found zero occurrences carrying old=GENERAL or old=VIDEO_ENCODE_SRC_KHR.
//
// WHY THIS ONE FLAG REACHES BOTH. The path is chosen by the registration's
// declared usage/tiling (ARM_DIRECT resolves DIRECT, ARM_FILTER_* resolve
// FILTER); the RESIDENCY is an independent axis this suite had pinned to LOCAL.
// Flipping it to FOREIGN is therefore the whole difference, and it is honoured
// verbatim: VulkanVideoEncoderExtImpl::SubmitRegisteredFrame only DERIVES
// residency when handleType is not VK_IMAGE, and every row here registers a
// VK_IMAGE. So the same nine rows re-run with FOREIGN drive the filter release
// on the six FILTER rows and Path A's release on the three DIRECT rows.
//
// VK_IMAGE IS THE RIGHT VEHICLE, not a compromise for a missing dma-buf. The
// image is created by this suite on the session device with no external memory,
// so the validation layer tracks its layout completely. A real dma-buf import
// brings VVL's external-image relaxations into play and can MASK exactly the
// layout inconsistency this mode exists to expose.
//
// The only extra requirement declaring FOREIGN imposes is that
// VK_EXT_queue_family_foreign be present (ValidateImageDescriptor); it is, on
// this hardware, which is what lets --foreign-opaque-fd pass today.
//
// WHAT IT REPORTS -- READ THIS BEFORE TREATING A NON-ZERO EXIT AS A
// REGRESSION. At 12 frames per row:
//
//   FILTER arm (5 rows, 60 frames): GREEN. The release records
//   (old=GENERAL new=GENERAL) on every frame and every row encodes
//   BYTE-IDENTICAL output to the RESIDENCY_LOCAL run (1930, 3162, 1930, 1930,
//   1930). This site had never executed before; it is correct as written.
//
//   DIRECT arm / Path A (2 rows, 24 frames): RED, AND STILL OPEN. The release
//   records (old=VIDEO_ENCODE_SRC_KHR new=GENERAL) and the session then dies:
//   VK_ERROR_DEVICE_LOST, 24 x VUID-vkResetFences-pFences-01123, and a
//   ZERO-BYTE bitstream on both rows where RESIDENCY_LOCAL produces 2011 and
//   3471 bytes. So --foreign-residency EXITS NON-ZERO BY DESIGN today; the
//   two DIRECT failures are the finding, not a broken suite.
//
// WHAT THE HANG IS NOT -- each of these was tried and measured, and none of
// them changed the outcome:
//   * NOT the release/acquire layout disagreement. Path A's release used to
//     hardcode VIDEO_ENCODE_SRC_KHR as its newLayout while the next acquire
//     declared GENERAL; that is now fixed (VkVideoEncoder::RecordVideoCodingCmd,
//     pathAProducerLayout) and confirmed via the [QFOT-REL] record, and the
//     DEVICE_LOST is identical before and after.
//   * NOT missing external memory. Backing the DIRECT images with
//     VkExternalMemoryImageCreateInfo + VkExportMemoryAllocateInfo (OPAQUE_FD)
//     changed nothing. That experiment was reverted rather than kept, because
//     it bought no behaviour.
//   * NOT the acquire. Suppressing ONLY the Path A release and keeping the
//     acquire makes the NV12 row encode 2011 bytes with zero errors -- so the
//     release barrier alone is the trigger.
//
// IT HAS NOW BEEN CHASED, AND IT IS NOT A LIBRARY DEFECT. The two bullets
// above survive, but they were "what it is not"; the cause is below and it is
// in the DRIVER. Isolated to a standalone program that links no part of this
// library, creates no video session, records no encode, and still loses the
// device.
//
// THE DEFECT, stated as a conjunction. A vkCmdPipelineBarrier2 image barrier
// loses the device iff ALL THREE hold:
//   (1) the image was created with VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR or
//       VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR. A VkVideoProfileListInfoKHR
//       WITHOUT either usage bit is green, so it is the usage, not the
//       profile, and not the multi-planar format (a plain
//       TRANSFER_SRC/DST NV12 image is green);
//   (2) the barrier is a RELEASE -- dstQueueFamilyIndex ==
//       VK_QUEUE_FAMILY_FOREIGN_EXT. VK_QUEUE_FAMILY_EXTERNAL in the same
//       position is green, a real family index is green, and the opposite
//       direction (a FOREIGN acquire) is green;
//   (3) it is recorded on a queue family other than graphics (family 0) or
//       optical flow (family 5). Families 1 (transfer), 2 (compute),
//       3 (video decode) and 4 (video encode) all die.
//
// INDEPENDENT OF: the layout pair (GENERAL->GENERAL, i.e. no transition at
// all, dies), srcStageMask (VIDEO_ENCODE, TRANSFER, ALL_COMMANDS and NONE all
// die), whether the memory is externally allocated and exportable, whether an
// acquire preceded it, whether any video command was ever recorded, and the
// barrier API generation -- the v1 vkCmdPipelineBarrier spelling of the same
// release dies identically, so this cannot be worked around by re-expressing
// the barrier.
//
// The failure is Xid 32 (invalid/corrupted push-buffer stream) on the
// recording engine's channel, HCE_DBG0 00000124 then 00000800.
// VK_EXT_device_fault is supported and returns an EMPTY fault record --
// consistent with a method-parse error rather than a memory fault.
//
// WHY THE LIBRARY CANNOT SIMPLY MOVE IT. A release must be recorded on a queue
// of its SOURCE family, and the only family that owns the Path-A input image
// is the encode family. So VkVideoEncoder::RecordVideoCodingCmd's release has
// nowhere legal to go, and the four possible responses -- drop the Path-A
// release, substitute VK_QUEUE_FAMILY_EXTERNAL (semantically wrong: it means
// another Vulkan instance, not a non-Vulkan agent), a two-hop
// encode->graphics->FOREIGN transfer on a second queue, or fix the driver --
// are a design decision, not a bug fix. NOTHING HAS BEEN CHANGED HERE, and
// --foreign-residency still EXITS NON-ZERO BY DESIGN on the two DIRECT rows.
bool g_foreignResidency = false;

// TRANSFER_SRC_OPTIMAL is legal only for an image created with
// VK_IMAGE_USAGE_TRANSFER_SRC_BIT (VUID-VkImageMemoryBarrier2-oldLayout-01208
// and the newLayout equivalent). ARM_FILTER_RGBA is declared STORAGE|
// TRANSFER_DST and has no TRANSFER_SRC, so it keeps declaring GENERAL rather
// than have this mode manufacture a validation error that says nothing about
// the restore. ARM_FILTER_YCBCR -- the three I420 rows, which are the filter
// consumers this mode exists for -- does carry it.
VkImageLayout DeclaredLayoutFor(VkImageUsageFlags usage)
{
    if (g_declareTso && (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

// ---------------------------------------------------------------------------
// The four-quadrant primaries pattern. PURE primaries, because a U/V swap
// conserves the byte histogram and a black conversion has the right size --
// neither a checksum nor a size check can catch either, and a mid-tone ramp
// makes both survivable. These exact values are the calibrated set.
// ---------------------------------------------------------------------------
struct RGB { uint8_t r, g, b; };
const RGB kQuadTL = {253,   0,   0};   // red
const RGB kQuadTR = {  0, 253,   0};   // green
const RGB kQuadBL = {  0,   0, 252};   // blue
const RGB kQuadBR = {255, 255, 255};   // white

RGB QuadAt(uint32_t x, uint32_t y)
{
    const bool right  = (x >= kWidth / 2);
    const bool bottom = (y >= kHeight / 2);
    if (!bottom) return right ? kQuadTR : kQuadTL;
    return right ? kQuadBR : kQuadBL;
}

// BT.709, LIMITED (studio) range -- the pair the session is configured with
// below (matrixCoefficients = 1, videoFullRange = VK_FALSE). Writing the
// YCbCr arms with the same matrix the RGBA arm's filter is told to use is
// what lets ONE out-of-process comparison judge every row.
struct YUV { double y, cb, cr; };
YUV RgbToYuv709Limited(const RGB& c)
{
    const double Kr = 0.2126, Kb = 0.0722;
    const double R = c.r, G = c.g, B = c.b;
    const double Yf = Kr * R + (1.0 - Kr - Kb) * G + Kb * B;   // 0..255
    YUV o;
    o.y  = 16.0  + 219.0 * Yf / 255.0;
    o.cb = 128.0 + 224.0 * (B - Yf) / (2.0 * (1.0 - Kb) * 255.0);
    o.cr = 128.0 + 224.0 * (R - Yf) / (2.0 * (1.0 - Kr) * 255.0);
    return o;
}

uint8_t Clamp8(double v)
{
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

uint16_t ClampN(double v, int bits)
{
    const double maxv = (double)((1 << bits) - 1);
    if (v < 0.0) return 0;
    if (v > maxv) return (uint16_t)maxv;
    return (uint16_t)(v + 0.5);
}

// ---------------------------------------------------------------------------
// Vulkan entry points, loaded off the LIBRARY's instance/device.
// ---------------------------------------------------------------------------
struct DeviceFns {
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkMapMemory                         MapMemory = nullptr;
    PFN_vkUnmapMemory                       UnmapMemory = nullptr;
    PFN_vkCreateBuffer                      CreateBuffer = nullptr;
    PFN_vkDestroyBuffer                     DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements       GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory                  BindBufferMemory = nullptr;
    PFN_vkCreateCommandPool                 CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool                DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers            AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer                BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer                  EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier                CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage              CmdCopyBufferToImage = nullptr;
    PFN_vkQueueSubmit                       QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle                     QueueWaitIdle = nullptr;
    PFN_vkGetDeviceQueue                    GetDeviceQueue = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties
        GetPhysicalDeviceQueueFamilyProperties = nullptr;
};

bool LoadDeviceFns(VkInstance instance, VkDevice device, DeviceFns* fns)
{
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (lib == nullptr) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (lib == nullptr) { std::printf("  ERROR: dlopen(libvulkan): %s\n", dlerror()); return false; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (gipa == nullptr) return false;
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (gdpa == nullptr) return false;
#define LOAD_DEV(name)                                                        \
    fns->name = (PFN_vk##name)gdpa(device, "vk" #name);                       \
    if (fns->name == nullptr) { std::printf("  ERROR: missing vk" #name "\n"); return false; }
    LOAD_DEV(CreateImage) LOAD_DEV(DestroyImage) LOAD_DEV(GetImageMemoryRequirements)
    LOAD_DEV(AllocateMemory) LOAD_DEV(FreeMemory) LOAD_DEV(BindImageMemory)
    LOAD_DEV(MapMemory) LOAD_DEV(UnmapMemory)
    LOAD_DEV(CreateBuffer) LOAD_DEV(DestroyBuffer) LOAD_DEV(GetBufferMemoryRequirements)
    LOAD_DEV(BindBufferMemory)
    LOAD_DEV(CreateCommandPool) LOAD_DEV(DestroyCommandPool) LOAD_DEV(AllocateCommandBuffers)
    LOAD_DEV(BeginCommandBuffer) LOAD_DEV(EndCommandBuffer)
    LOAD_DEV(CmdPipelineBarrier) LOAD_DEV(CmdCopyBufferToImage)
    LOAD_DEV(QueueSubmit) LOAD_DEV(QueueWaitIdle) LOAD_DEV(GetDeviceQueue)
#undef LOAD_DEV
    fns->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    fns->GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    fns->GetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    return fns->GetPhysicalDeviceMemoryProperties && fns->GetPhysicalDeviceProperties &&
           fns->GetPhysicalDeviceQueueFamilyProperties;
}

// --nv12-companion. THE SHAPE CHROMIUM NOW BUILDS, and the one shape this
// matrix cannot express on its own: a session DECLARED in an RGBA format
// that is also handed NV12 descriptors.
//
// Chromium's VEA declares the session from its widest admissible input format
// rather than from the frame format its embedder asked for, because
// VideoEncodeAcceleratorAdapter pins that to NV12 before any frame exists and a
// session declared NV12 has no filter at all. The consequence is that an
// ordinary NV12 stream now runs on a session whose enablePreprocessFilter is
// VK_TRUE -- and GetStagedInputSubmitType() returns COMPUTE for exactly such a
// session, so every staged NV12 frame moves off the encode/transfer family onto
// the compute family. That is a correct consequence of
// VUID-vkQueueSubmit2-commandBuffer-03874, and it is still a behaviour change
// on the busiest lane in the product, on a driver that has already been
// measured losing the device over a queue-family mistake on the input
// acquire/release path.
//
// So this mode exists to MEASURE two claims that were otherwise only reasoned
// about:
//   1. the library's registration gate really does admit BOTH the declared
//      filter-input format and the recomputed semi-planar encode-source format
//      on ONE session -- and routes them FILTER and DIRECT respectively; and
//   2. an NV12 frame on such a session encodes with zero spec violations under
//      --validate, i.e. the compute-family move is clean.
//
// It runs ONLY on the ARM_FILTER_RGBA rows and only under the flag, so every
// standing bar of this suite is byte-identical without it.
bool g_nv12Companion = false;

// --nv12-staged-companion. THE SHAPE CHROMIUM ACTUALLY PRODUCES, which is NOT
// the shape --nv12-companion measures, and the difference is the whole point.
//
// --nv12-companion declares its NV12 companion OPTIMAL + VIDEO_ENCODE_SRC, so
// the registration resolves DIRECT: the encode reads the caller's image in
// place and StageInputFrame is never entered. Chromium's shipping NV12 lanes
// resolve STAGED instead, for two independently measured reasons:
//
//   * tier 2 (CPU dma-buf, ENABLED BY DEFAULT and the only zero-copy-adjacent
//     tier X11 can reach): for a modifier-0 NV12 dma-buf, declaring
//     VIDEO_ENCODE_SRC makes the library answer MODIFIER_UNSUPPORTED and the
//     driver agree with VK_ERROR_FORMAT_NOT_SUPPORTED, so the VEA declares
//     TRANSFER_SRC alone and encodeCapable resolves 0; and
//   * tier 3 (OPAQUE_FD staging): a LINEAR host-written image, TRANSFER_SRC
//     alone by construction.
//
// Either way the descriptor the library sees is LINEAR-or-non-encode-capable
// NV12 with TRANSFER_SRC usage, it routes STAGED, and it takes the plain-COPY
// arm inside StageInputFrame -- and that arm's submit family is read from
// GetStagedInputSubmitType(), which returns COMPUTE for any session carrying a
// preprocess filter OBJECT. A session declared RGBA always carries one. So
// widening the declared session format silently migrates the busiest lane in
// the product, including its FOREIGN queue-family RELEASE, from the encode
// family onto the compute family -- on a driver that loses the device on a
// FOREIGN release off the wrong engine.
//
// This mode is the A/B for that. It attaches the SAME LINEAR/TRANSFER_SRC NV12
// companion to two sessions:
//   arm A -- the NV12-declared row: no filter, so the copy stays on the
//            encode (or transfer) family. The control.
//   arm B -- each RGBA-declared row: filter present, so the copy moves to the
//            compute family. The subject.
// and prints, per arm, the submit-type queue flags and the queue-family index
// the library ACTUALLY used, read back through
// VkVideoEncoderInputResidencyInfo rather than re-derived here.
bool g_stagedCompanion = false;

// --companion-foreign. RESIDENCY_FOREIGN ON THE COMPANION ONLY, and the
// isolation is the point rather than a convenience.
//
// The staged copy arm's ReleaseImageToForeignQueue only runs for a frame whose
// residency resolves FOREIGN, so the queue-family question cannot be answered
// at all under the default RESIDENCY_LOCAL -- measured, foreign=0 local=12 on
// both arms of the A/B. The obvious lever, --foreign-residency, is unusable
// here: it also flips the PRIMARY registration, and on a DIRECT row that is a
// KNOWN VK_ERROR_DEVICE_LOST plus Xid 32 through Path A's own release (see
// g_foreignResidency). The process would die
// before the companion registered, and a device loss caused by a different
// barrier is not evidence about this one.
//
// So this flag drives the companion's residency alone. The primary stays LOCAL,
// Path A never releases, and the ONLY FOREIGN release in the run is the staged
// copy arm's release of the companion's LINEAR/TRANSFER_SRC image -- on family 4
// in arm A and family 0 in arm B. That is exactly one variable.
bool g_companionForeign = false;

enum Arm { ARM_DIRECT, ARM_FILTER_YCBCR, ARM_FILTER_RGBA };
enum Group { G_8BIT = 0, G_10BIT = 1, G_12BIT = 2 };

struct Row {
    const char* name;
    const char* shortName;
    VkFormat    format;
    Arm         arm;
    Group       group;
    VkVideoCodecOperationFlagBitsKHR codec;
    const char* ext;
};

#define H264 VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "264"
#define H265 VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "265"
// AV1 -- AND THE EXTENSION IS "obu", NOT "ivf". THAT IS A MEASUREMENT AND A
// RECORDED COVERAGE GAP, not a preference.
//
// VkVideoEncoderAV1 has TWO output arms and they emit DIFFERENT BYTES:
//
//   FILE arm (disableFileOutput == VK_FALSE) --
//     VkVideoEncoderAV1::WriteBitstreamToFileOutput -> BuildFrameObuSequence
//     -> FlushBatchedTemporalUnit, which muxes the 32-byte DKIF/'AV01' IVF
//     file header and a 12-byte IVF frame header per temporal unit
//     (VkVideoEncoderAV1.cpp:854-925, 1059-1079).
//
//   CAPTURE arm (disableFileOutput == VK_TRUE) --
//     the in-memory branch of VkVideoEncoderAV1::WriteBitstreamToFile
//     (VkVideoEncoderAV1.cpp:1026-1045). It prepends the two-byte Temporal
//     Delimiter OBU {0x12,0x00}, appends the sequence-header OBU when the
//     frame carries one, appends the frame OBU, and publishes THAT as the
//     completion record. It never enters FlushBatchedTemporalUnit and writes
//     no IVF header of any kind.
//
// THIS SUITE SETS disableFileOutput = VK_TRUE and drains through
// AcquireNextEncodedFrame, so it takes the CAPTURE arm -- and so does
// Chromium, for the same reason. What reaches the file this harness writes is
// a bare low-overhead OBU stream: TD-delimited temporal units, no container.
// Calling it ".ivf" would be false, and would imply the IVF muxer had run.
//
// SO THE GAP, PLAINLY: BuildFrameObuSequence and FlushBatchedTemporalUnit --
// the DKIF/AV01 construction the AV1 half of the upstream refactor was
// accepted on a STATIC argument -- are NOT reachable from this harness,
// because they sit on the arm neither this suite nor the product takes.
// Covering them needs a file-output AV1 session, which is a different
// harness. This one must not claim them.
//
// The capture arm's shape needs no demuxer hint:
//   * an SVT-AV1 stream muxed with `-f obu` begins 12 00 0a 0e ... -- the
//     same {0x12,0x00} TD OBU this arm prepends, so the shapes agree;
//   * ffprobe resolves it format_name=obu codec_name=av1 with NO -f flag, and
//     STILL resolves it as obu when the identical bytes are renamed .ivf --
//     the content probe outranks the extension in both directions;
//   * the decode gate's verbatim command line decodes it, and all twelve
//     concatenated temporal units are read back (nb_read_frames=12).
// Hence NO `-f obu` below: it was tried and it is not needed. If a future
// ffmpeg regresses the obu probe, adding it for these rows is the one-line
// fix -- but adding it today would be cargo cult.
#define AV1  VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,  "obu"

const Row kRows[] = {
    {"NV12    G8_B8R8_2PLANE_420_UNORM",       "nv12",    VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   ARM_DIRECT,       G_8BIT,  H264},
    {"P010    G10X6_B10X6R10X6_2PLANE_420",    "p010",    VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,  ARM_DIRECT,       G_10BIT, H265},
    {"P012    G12X4_B12X4R12X4_2PLANE_420",    "p012",    VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,  ARM_DIRECT,       G_12BIT, H265},
    {"I420    G8_B8_R8_3PLANE_420_UNORM",      "i420",    VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,                  ARM_FILTER_YCBCR, G_8BIT,  H264},
    {"I420-10 G10X6_B10X6_R10X6_3PLANE_420",   "i420p10", VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, ARM_FILTER_YCBCR, G_10BIT, H265},
    {"I420-12 G12X4_B12X4_R12X4_3PLANE_420",   "i420p12", VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16, ARM_FILTER_YCBCR, G_12BIT, H265},
    {"RGBA8   R8G8B8A8_UNORM",                 "rgba8",   VK_FORMAT_R8G8B8A8_UNORM,                             ARM_FILTER_RGBA,  G_8BIT,  H264},
    {"BGRA8   B8G8R8A8_UNORM",                 "bgra8",   VK_FORMAT_B8G8R8A8_UNORM,                             ARM_FILTER_RGBA,  G_8BIT,  H264},
    {"ABGR8   A8B8G8R8_UNORM_PACK32",          "abgr8",   VK_FORMAT_A8B8G8R8_UNORM_PACK32,                      ARM_FILTER_RGBA,  G_8BIT,  H264},
    // ---- HEVC MAIN, 8-BIT. The one advertised codec/depth pair that no row
    // reached, at either layer, and it was a hole in this table's shape rather
    // than a decision: the rows are indexed by INPUT FORMAT and the codec is a
    // dependent variable of bit depth, so H.264 took every 8-bit slot and
    // every H.265 slot needed a format above 8 bits. Same VkFormat as the NV12
    // row above ON PURPOSE -- the input path is identical and already proven,
    // so the ONLY variable this row adds is the codec, which is what makes it
    // a clean read. It is also the only row that executes
    // STD_VIDEO_H265_PROFILE_IDC_MAIN in CreateImage; that arm is dead
    // otherwise. Distinct shortName because the output stem is
    // "<outDir>/<shortName>.<ext>" and "nv12" is taken.
    {"NV12-265 G8_B8R8_2PLANE_420 (H.265 Main)", "nv12h265", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   ARM_DIRECT,       G_8BIT,  H265},
    // ---- AV1. THIS CODEC HAS NEVER EXECUTED ANYWHERE IN THIS TREE. -------
    //
    // WHICH ROWS, AND WHY EXACTLY THESE FOUR. The input-format taxonomy
    // (VkEncClassifyInputFormat) is CODEC-INDEPENDENT -- it takes a VkFormat
    // and nothing else -- so AV1 inherits the same nine-format ladder the
    // rows above walk. What narrows it is the PROFILE, and the library states
    // the constraint itself: AV1 Main is 8/10-bit 4:2:0 only, and 12-bit
    // input against it is refused with a reason
    // (vulkan_video_encoder_ext.cpp:1403-1428).
    //
    //   * P012 and I420-12 are EXCLUDED. Against AV1 Main they can only ever
    //     report a PROFILE refusal, which is a fact about the enum and not
    //     about the encoder or the device. A row whose only possible outcome
    //     is a refusal measures the row.
    //   * NV12 (8-bit) and P010 (10-bit) are the whole ENCODABLE_DIRECT arm
    //     AV1 Main admits, and both are here. 10-bit is not redundant: the
    //     AV1 sequence header carries its own colour config, whose BitDepth
    //     and colour fields are built by AV1-only code (the av1BitDepth /
    //     av1ColorRange probe fields in vulkan_video_encoder_ext.cpp:931-951
    //     exist precisely because nothing else reads them).
    //   * I420 is the ENCODABLE_VIA_FILTER *YCbCr* arm -- three planes into
    //     two, through the compute filter's per-plane storage-view read.
    //   * RGBA8 is the ENCODABLE_VIA_FILTER *RGBA* arm -- one plane, read
    //     through a single combined storage view and put through the RGB to
    //     Y'CbCr matrix. That is a different shader path from I420's, so one
    //     row cannot stand in for both.
    //   * BGRA8, ABGR8 and I420-10 are EXCLUDED as REDUNDANT, not as
    //     unsupported. What each adds over RGBA8 / I420 is which VkFormat the
    //     filter resolves its view from, and that is settled before the codec
    //     is consulted; it is already covered on the H.26x rows above.
    //
    // Both routing arms are therefore exercised under AV1 -- DIRECT, and both
    // sub-arms of FILTER -- which is the whole point: the AV1 encoder, the
    // seven ext-layer AV1 sites and VkVideoEncoderAV1.cpp have never seen a
    // frame from any of them.
    {"AV1-NV12  G8_B8R8_2PLANE_420_UNORM",     "av1nv12", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   ARM_DIRECT,       G_8BIT,  AV1},
    {"AV1-P010  G10X6_B10X6R10X6_2PLANE_420",  "av1p010", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,  ARM_DIRECT,       G_10BIT, AV1},
    {"AV1-I420  G8_B8_R8_3PLANE_420_UNORM",    "av1i420", VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,                  ARM_FILTER_YCBCR, G_8BIT,  AV1},
    {"AV1-RGBA8 R8G8B8A8_UNORM",               "av1rgba", VK_FORMAT_R8G8B8A8_UNORM,                             ARM_FILTER_RGBA,  G_8BIT,  AV1},
};
const size_t kNumRows = sizeof(kRows) / sizeof(kRows[0]);

// The codec, as the string --codec takes. Used only by the row filter and the
// report; kept beside the macros above so the two cannot drift.
const char* CodecName(VkVideoCodecOperationFlagBitsKHR c)
{
    switch ((int)c) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: return "h264";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: return "h265";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:  return "av1";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// DEVICE LIMITATION vs LIBRARY REFUSAL, and why that has to be decided in
// code rather than left to whoever reads the log.
//
// A row that gets no session prints one line and is not counted as a failure.
// That is right for P012 and I420-12 on hardware with no 12-bit encode
// profile -- and it is exactly the shape in which a NEW arm passes without
// ever running. The AV1 rows are the first arm in this file that the
// reference hardware (RTX A4000, GA104) cannot run AT ALL, so the distinction
// stops being cosmetic and becomes the difference between a gate and a
// decoration.
//
// WHAT THE A4000 ANSWERS FOR AV1, traced through the library rather than
// guessed. VK_KHR_video_encode_av1 is on the OPTIONAL device-extension list
// (vulkan_video_encoder_ext.cpp:1615), so HasAllDeviceExtensions() warns and
// DROPS it rather than failing device selection -- the row does not die
// there. It dies one step later, in VulkanDeviceContext::InitPhysicalDevice:
// the encode-queue clause admits a family only when
// `videoQueue.videoCodecOperations & requestVideoEncodeQueueOperations`
// (VulkanDeviceContext.cpp:857-859); the A4000's encode family advertises
// H.264 and H.265 and not AV1; no family matches, no physical device is
// selected, and the function returns VK_ERROR_FEATURE_NOT_PRESENT
// (VulkanDeviceContext.cpp:972). So -8 is the expected AV1 answer there.
//
// The codes below are the ones that mean THE DEVICE SAID NO. Everything else
// -- notably VK_ERROR_INITIALIZATION_FAILED, which is what every ext-layer
// refusal in BuildEncoderConfig returns -- is the LIBRARY saying no with a
// driver in hand, which is a verdict and not an environment fact. That is the
// sentence CMakeLists.txt already applies to nSession == 0, one level finer.
bool IsDeviceLimitedInit(VkResult r)
{
    switch ((int)r) {
        case VK_ERROR_FEATURE_NOT_PRESENT:                        // -8
        case VK_ERROR_EXTENSION_NOT_PRESENT:                      // -7
        case VK_ERROR_FORMAT_NOT_SUPPORTED:                       // -11
        case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR:              // -1000023000
        case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:     // -1000023001
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:  // -1000023002
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:     // -1000023003
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:      // -1000023004
        case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:        // -1000023005
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// THE AV1 ANTI-SILENT-PASS PROBE. This is what makes "device-limited" a
// finding rather than an assumption.
//
// "No session, therefore this device cannot do AV1" is not an inference. It
// is the assumption that turns a new arm into a test that cannot fail: if the
// AV1 path is broken on hardware that HAS AV1, the row still reports no
// session, still is not counted, and the suite still exits 0. That is the
// thirteenth cannot-fail test, pre-built, and this function is the reason it
// is not one.
//
// So the claim is CHECKED against the device, independently of the library: a
// throwaway VkInstance of this test's own, then
// vkEnumerateDeviceExtensionProperties on each physical device, looking for
// VK_KHR_video_encode_av1. The extension STRING is the primary signal on
// purpose -- it is a flat list with no pNext chaining, so it cannot come back
// silently empty the way a feature struct an older driver does not recognise
// can, which is exactly the trap
// VkPhysicalDeviceVideoEncodeAV1FeaturesKHR would set here.
//
// The three answers, and what each licenses:
//   present (1) -- a no-session AV1 row is a FAILURE. The device advertises
//                  the extension, so something between that and the session
//                  is broken; surfacing that is what this arm is for.
//   absent  (0) -- a no-session AV1 row carrying a device-limitation code is
//                  DEVICE-LIMITED, and is not a failure. This is the A4000.
//   unknown(-1) -- no instance could be made. Reported UNVERIFIED and
//                  counted; never a clean pass.
//
// The over-approximation points the safe way ON PURPOSE: this asks whether
// ANY physical device advertises AV1 encode, while the session selects one.
// On a multi-GPU host that can only convert a silent pass into a loud
// failure, never the reverse.
int         g_av1Probe = -1;      // -1 unknown, 0 absent, 1 present
std::string g_av1ProbeDetail;

// VK_DRIVER_FILES / VK_ICD_FILENAMES scope the Vulkan loader for the WHOLE
// process. When either is set, EVERY instance in this process -- the probe's
// and the library's alike -- is offered a SUBSET of the machine's drivers, so
// a short device list is the environment speaking, not the probe failing.
// Returns nullptr when the loader is unscoped.
const char* Av1ProbeIcdScope()
{
    const char* s = std::getenv("VK_DRIVER_FILES");
    if ((s == nullptr) || (s[0] == '\0')) s = std::getenv("VK_ICD_FILENAMES");
    return ((s != nullptr) && (s[0] != '\0')) ? s : nullptr;
}

void ProbeAv1EncodeSupport()
{
    if (!g_av1ProbeDetail.empty()) return;   // probed at most once per run
    g_av1ProbeDetail = "the probe did not complete";

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (lib == nullptr) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (lib == nullptr) { g_av1ProbeDetail = "dlopen(libvulkan) failed"; return; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (gipa == nullptr) { g_av1ProbeDetail = "no vkGetInstanceProcAddr"; return; }
    auto createInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    if (createInstance == nullptr) { g_av1ProbeDetail = "no vkCreateInstance"; return; }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "encoder-ext-format-encode av1 probe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    const VkResult ir = createInstance(&ici, nullptr, &inst);
    if ((ir != VK_SUCCESS) || (inst == VK_NULL_HANDLE)) {
        char m[160];
        std::snprintf(m, sizeof(m),
                      "vkCreateInstance -> %d, so no device could be asked",
                      (int)ir);
        g_av1ProbeDetail = m;
        return;
    }

    auto destroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    auto enumPhys   = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto enumDevExt = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    auto getProps   = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    if ((enumPhys == nullptr) || (enumDevExt == nullptr)) {
        g_av1ProbeDetail = "instance entry points missing";
        if (destroyInstance != nullptr) destroyInstance(inst, nullptr);
        return;
    }

    uint32_t nDev = 0;
    enumPhys(inst, &nDev, nullptr);
    std::vector<VkPhysicalDevice> devs(nDev);
    if (nDev != 0) enumPhys(inst, &nDev, devs.data());
    if (nDev == 0) {
        {
            const char* scope = Av1ProbeIcdScope();
            char z[512];
            std::snprintf(z, sizeof(z),
                          "the instance enumerated no physical device%s%s%s",
                          (scope != nullptr) ? " (loader scoped by VK_DRIVER_FILES/VK_ICD_FILENAMES to " : "",
                          (scope != nullptr) ? scope : "",
                          (scope != nullptr) ? ")" : "");
            g_av1ProbeDetail = z;
        }
        if (destroyInstance != nullptr) destroyInstance(inst, nullptr);
        return;
    }

    std::string detail;
    int found = 0;
    for (uint32_t d = 0; d < nDev; d++) {
        uint32_t nExt = 0;
        enumDevExt(devs[d], nullptr, &nExt, nullptr);
        std::vector<VkExtensionProperties> exts(nExt);
        if (nExt != 0) enumDevExt(devs[d], nullptr, &nExt, exts.data());
        bool hasAv1 = false;
        for (uint32_t e = 0; e < nExt; e++) {
            if (std::strcmp(exts[e].extensionName,
                            VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME) == 0) {
                hasAv1 = true;
                break;
            }
        }
        if (hasAv1) found = 1;
        VkPhysicalDeviceProperties p{};
        if (getProps != nullptr) getProps(devs[d], &p);
        char one[320];
        std::snprintf(one, sizeof(one), "%s%s %s VK_KHR_video_encode_av1",
                      detail.empty() ? "" : "; ",
                      (getProps != nullptr) ? p.deviceName : "device",
                      hasAv1 ? "HAS" : "does NOT enumerate");
        detail += one;
    }
    // THE COUNT IS PART OF THE VERDICT, not decoration. A verdict that names
    // one device is ambiguous between "the probe truncated its report" and
    // "this process's loader was only ever offered one driver", and that
    // ambiguity has already cost a misdiagnosis: a CORRECT llvmpipe-only
    // verdict was read as a probe defect and sent someone looking for a bug
    // in vkCreateInstance parameters that was never there. Stating the count,
    // and stating when the loader was scoped, collapses the ambiguity at the
    // point of reading instead of leaving it for a bisect.
    {
        const char* scope = Av1ProbeIcdScope();
        char head[640];
        std::snprintf(head, sizeof(head), "%u device%s enumerated%s%s%s: ",
                      nDev, (nDev == 1) ? "" : "s",
                      (scope != nullptr) ? " (loader scoped by VK_DRIVER_FILES/VK_ICD_FILENAMES to " : "",
                      (scope != nullptr) ? scope : "",
                      (scope != nullptr) ? ")" : "");
        detail = std::string(head) + detail;
    }
    g_av1Probe = found;
    g_av1ProbeDetail = detail;
    if (destroyInstance != nullptr) destroyInstance(inst, nullptr);
}

const char* PathName(VkVideoEncoderExternalInputPath p)
{
    switch (p) {
        case VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT: return "DIRECT";
        case VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED: return "STAGED";
        case VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER: return "FILTER";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Pattern writers. One per format family; each fills a tightly packed
// staging buffer laid out plane-after-plane and reports the plane offsets.
// ---------------------------------------------------------------------------
struct PlaneCopy { VkDeviceSize offset; uint32_t w, h; VkImageAspectFlagBits aspect; };

size_t BuildPattern(VkFormat fmt, std::vector<uint8_t>* buf,
                    std::vector<PlaneCopy>* planes)
{
    buf->clear();
    planes->clear();
    const uint32_t cw = kWidth / 2, ch = kHeight / 2;

    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: {
            // A8B8G8R8_UNORM_PACK32 is a uint32 A<<24|B<<16|G<<8|R, which on
            // a little-endian host is byte order R,G,B,A -- BYTE-IDENTICAL to
            // R8G8B8A8_UNORM. Written once for both, deliberately: if these
            // two rows ever decode differently the difference is in the view
            // format the filter resolves, not in what was uploaded.
            buf->resize((size_t)kWidth * kHeight * 4);
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++) {
                    const RGB c = QuadAt(x, y);
                    uint8_t* p = buf->data() + ((size_t)y * kWidth + x) * 4;
                    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_COLOR_BIT});
            break;
        }
        case VK_FORMAT_B8G8R8A8_UNORM: {
            buf->resize((size_t)kWidth * kHeight * 4);
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++) {
                    const RGB c = QuadAt(x, y);
                    uint8_t* p = buf->data() + ((size_t)y * kWidth + x) * 4;
                    p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 255;
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_COLOR_BIT});
            break;
        }
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: {   // NV12
            const size_t ySize = (size_t)kWidth * kHeight;
            buf->resize(ySize + (size_t)cw * ch * 2);
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++)
                    (*buf)[(size_t)y * kWidth + x] = Clamp8(RgbToYuv709Limited(QuadAt(x, y)).y);
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++) {
                    const YUV c = RgbToYuv709Limited(QuadAt(x * 2, y * 2));
                    uint8_t* p = buf->data() + ySize + ((size_t)y * cw + x) * 2;
                    p[0] = Clamp8(c.cb); p[1] = Clamp8(c.cr);
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_PLANE_0_BIT});
            planes->push_back({(VkDeviceSize)ySize, cw, ch, VK_IMAGE_ASPECT_PLANE_1_BIT});
            break;
        }
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:    // P010
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: {  // P012
            const int bits  = (fmt == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) ? 10 : 12;
            const int shift = g_rawBits ? 0 : (16 - bits);   // value sits in the HIGH bits
            const double scale = (double)((1 << bits) - 1) / 255.0;
            const size_t ySize = (size_t)kWidth * kHeight * 2;
            buf->resize(ySize + (size_t)cw * ch * 2 * 2);
            uint16_t* yp = (uint16_t*)buf->data();
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++)
                    yp[(size_t)y * kWidth + x] =
                        (uint16_t)(ClampN(RgbToYuv709Limited(QuadAt(x, y)).y * scale, bits) << shift);
            uint16_t* cp = (uint16_t*)(buf->data() + ySize);
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++) {
                    const YUV c = RgbToYuv709Limited(QuadAt(x * 2, y * 2));
                    cp[((size_t)y * cw + x) * 2 + 0] = (uint16_t)(ClampN(c.cb * scale, bits) << shift);
                    cp[((size_t)y * cw + x) * 2 + 1] = (uint16_t)(ClampN(c.cr * scale, bits) << shift);
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_PLANE_0_BIT});
            planes->push_back({(VkDeviceSize)ySize, cw, ch, VK_IMAGE_ASPECT_PLANE_1_BIT});
            break;
        }
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: {   // I420
            const size_t ySize = (size_t)kWidth * kHeight, cSize = (size_t)cw * ch;
            buf->resize(ySize + cSize * 2);
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++)
                    (*buf)[(size_t)y * kWidth + x] = Clamp8(RgbToYuv709Limited(QuadAt(x, y)).y);
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++) {
                    const YUV c = RgbToYuv709Limited(QuadAt(x * 2, y * 2));
                    (*buf)[ySize + (size_t)y * cw + x]         = Clamp8(c.cb);
                    (*buf)[ySize + cSize + (size_t)y * cw + x] = Clamp8(c.cr);
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_PLANE_0_BIT});
            planes->push_back({(VkDeviceSize)ySize, cw, ch, VK_IMAGE_ASPECT_PLANE_1_BIT});
            planes->push_back({(VkDeviceSize)(ySize + cSize), cw, ch, VK_IMAGE_ASPECT_PLANE_2_BIT});
            break;
        }
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: {
            const int bits  = (fmt == VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16) ? 10 : 12;
            const int shift = g_rawBits ? 0 : (16 - bits);
            const double scale = (double)((1 << bits) - 1) / 255.0;
            const size_t ySize = (size_t)kWidth * kHeight * 2, cSize = (size_t)cw * ch * 2;
            buf->resize(ySize + cSize * 2);
            uint16_t* yp = (uint16_t*)buf->data();
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++)
                    yp[(size_t)y * kWidth + x] =
                        (uint16_t)(ClampN(RgbToYuv709Limited(QuadAt(x, y)).y * scale, bits) << shift);
            uint16_t* bp = (uint16_t*)(buf->data() + ySize);
            uint16_t* rp = (uint16_t*)(buf->data() + ySize + cSize);
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++) {
                    const YUV c = RgbToYuv709Limited(QuadAt(x * 2, y * 2));
                    bp[(size_t)y * cw + x] = (uint16_t)(ClampN(c.cb * scale, bits) << shift);
                    rp[(size_t)y * cw + x] = (uint16_t)(ClampN(c.cr * scale, bits) << shift);
                }
            planes->push_back({0, kWidth, kHeight, VK_IMAGE_ASPECT_PLANE_0_BIT});
            planes->push_back({(VkDeviceSize)ySize, cw, ch, VK_IMAGE_ASPECT_PLANE_1_BIT});
            planes->push_back({(VkDeviceSize)(ySize + cSize), cw, ch, VK_IMAGE_ASPECT_PLANE_2_BIT});
            break;
        }
        default:
            return 0;
    }
    return buf->size();
}

// The ext header's own enumerator spellings, so a log line greps straight
// into VkVideoEncoderImportContentState.
const char* ContentStateName(uint32_t state)
{
    switch ((VkVideoEncoderImportContentState)state) {
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_EVALUATED:  return "NOT_EVALUATED";
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE: return "NOT_APPLICABLE";
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED:          return "ARMED";
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_CLEAN:          return "CLEAN";
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_CHROMA: return "DAMAGED_CHROMA";
        case VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_DAMAGED_ALL:    return "DAMAGED_ALL";
    }
    return "unknown";
}

struct Decl { VkImageUsageFlags usage; VkImageCreateFlags flags; VkImageTiling tiling; bool profileList; };

Decl DeclFor(Arm arm)
{
    Decl d = {};
    d.tiling = VK_IMAGE_TILING_OPTIMAL;
    switch (arm) {
        case ARM_DIRECT:
            // TRANSFER_DST is added to every arm purely so the pattern can be
            // uploaded. It is a superset of the declaration the routing test
            // validated; each row prints its resolved path so a routing change
            // caused by this addition would be visible rather than assumed.
            d.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            d.profileList = true;
            break;
        case ARM_FILTER_YCBCR:
            d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            d.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            break;
        case ARM_FILTER_RGBA:
            // The filter's single-plane arm binds this image's ONE combined
            // view as a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, so STORAGE is the
            // usage it needs -- and no create flags.
            d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            break;
    }
    return d;
}

// The NV12 companion's declaration. Deliberately NOT an Arm case: an Arm
// describes how a SESSION is configured, and this describes a registration
// attached to somebody else's session.
//
// LINEAR + TRANSFER_SRC and NO video profile list, which together are what
// make slot.encodeCapable resolve 0 (it requires OPTIMAL tiling AND
// VIDEO_ENCODE_SRC) and therefore what make the registration route STAGED.
// TRANSFER_DST is present only so UploadPattern can write the pattern, exactly
// as it is on every other arm.
//
// Consequence worth naming, because it is the reason the arm-B measurement is
// not simply a repeat of the Path-A device loss: this image carries NO
// VIDEO_ENCODE_SRC and no VIDEO_ENCODE_DPB usage. The driver defect is gated on
// that usage (`rel --novideo` SURVIVED in the reproducer's matrix), so the
// prediction going in is that the compute-family FOREIGN release here is
// harmless. A prediction is not a measurement, which is why this mode exists.
Decl StagedCompanionDecl()
{
    Decl d = {};
    d.tiling = VK_IMAGE_TILING_LINEAR;
    d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.flags = 0;
    d.profileList = false;
    return d;
}

struct Img { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; };

bool CreateImage(const DeviceFns& fns, VkPhysicalDevice phys, VkDevice device,
                 VkFormat format, const Decl& d,
                 VkVideoCodecOperationFlagBitsKHR codec,
                 VkVideoComponentBitDepthFlagBitsKHR depth, Img* out)
{
    VkVideoEncodeH264ProfileInfoKHR h264Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    VkVideoEncodeH265ProfileInfoKHR h265Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR};
    h265Profile.stdProfileIdc = (depth == VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
        ? STD_VIDEO_H265_PROFILE_IDC_MAIN : STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
    // AV1's profile struct is NOT shaped like the H.26x pair, and the
    // difference is not cosmetic: the field is `stdProfile`, of type
    // StdVideoAV1Profile, and there is no `stdProfileIdc`. Matched to how the
    // LIBRARY builds it rather than invented -- VkVideoCoreProfile's AV1 arm
    // populates exactly {sType = ..._VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
    // stdProfile = STD_VIDEO_AV1_PROFILE_MAIN} for a default AV1 encode
    // profile (VkVideoCoreProfile.h:166-185 and 288-294).
    //
    // MAIN serves BOTH AV1 rows, and that is a decision, not a default: AV1
    // Main is 8/10-bit 4:2:0, which is exactly the two depths the AV1 rows
    // use -- unlike H.265 above, which has to pick MAIN vs MAIN_10. The
    // profile named in this image's VkVideoProfileListInfoKHR must be the one
    // the session will use, or vkCreateImage is being asked about an image
    // the encoder will not be able to read.
    VkVideoEncodeAV1ProfileInfoKHR av1Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR};
    av1Profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    // A SWITCH, NOT THE TWO-CODEC TERNARY THIS REPLACES. That ternary
    // answered "H.265" for every codec that was not H.264, so an AV1 row
    // would have been handed a VkVideoEncodeH265ProfileInfoKHR chained under
    // an AV1 videoCodecOperation -- a mismatch the driver would either refuse
    // for the wrong reason or, worse, accept.
    switch (codec) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
            profile.pNext = (void*)&h264Profile; break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
            profile.pNext = (void*)&h265Profile; break;
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR:
            profile.pNext = (void*)&av1Profile;  break;
        default:
            // Unreachable from kRows, and a hard stop rather than a silent
            // fallthrough for exactly the reason above.
            std::printf("  ERROR: no profile shape for codec 0x%x\n",
                        (unsigned)codec);
            return false;
    }
    profile.videoCodecOperation = codec;
    profile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = depth; profile.chromaBitDepth = depth;
    VkVideoProfileListInfoKHR profileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profileList.profileCount = 1; profileList.pProfiles = &profile;

    VkFormat viewFormats[4] = {format, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    uint32_t viewFormatCount = 1;
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
            viewFormats[viewFormatCount++] = VK_FORMAT_R8_UNORM; break;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
            viewFormats[viewFormatCount++] = VK_FORMAT_R10X6_UNORM_PACK16;
            viewFormats[viewFormatCount++] = VK_FORMAT_R16_UNORM; break;
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
            viewFormats[viewFormatCount++] = VK_FORMAT_R12X4_UNORM_PACK16;
            viewFormats[viewFormatCount++] = VK_FORMAT_R16_UNORM; break;
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
            viewFormats[viewFormatCount++] = VK_FORMAT_R8_UNORM;
            viewFormats[viewFormatCount++] = VK_FORMAT_R8G8_UNORM; break;
        default: break;
    }
    VkImageFormatListCreateInfo listInfo{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    listInfo.viewFormatCount = viewFormatCount; listInfo.pViewFormats = viewFormats;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const void* chain = nullptr;
    if (d.profileList) chain = &profileList;
    if ((d.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) { listInfo.pNext = chain; chain = &listInfo; }
    ci.pNext = chain; ci.flags = d.flags; ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format; ci.extent = {kWidth, kHeight, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = d.tiling; ci.usage = d.usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fns.CreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{}; fns.GetImageMemoryRequirements(device, out->image, &req);
    VkPhysicalDeviceMemoryProperties memProps{}; fns.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if (((req.memoryTypeBits & (1u << i)) != 0) &&
            ((memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)) { typeIndex = i; break; }
    if (typeIndex == UINT32_MAX) { fns.DestroyImage(device, out->image, nullptr); out->image = VK_NULL_HANDLE; return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = typeIndex;
    if (fns.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) {
        fns.DestroyImage(device, out->image, nullptr); out->image = VK_NULL_HANDLE; return false;
    }
    return fns.BindImageMemory(device, out->image, out->memory, 0) == VK_SUCCESS;
}

void DestroyImg(const DeviceFns& fns, VkDevice device, Img* img)
{
    if (img->image  != VK_NULL_HANDLE) { fns.DestroyImage(device, img->image, nullptr);  img->image  = VK_NULL_HANDLE; }
    if (img->memory != VK_NULL_HANDLE) { fns.FreeMemory(device, img->memory, nullptr);   img->memory = VK_NULL_HANDLE; }
}

// Upload the pattern. Runs on the session's VIDEO ENCODE queue family --
// which on this vendor advertises TRANSFER, and is a family the library
// demonstrably created a queue on (vkGetDeviceQueue on any other family
// would be undefined behavior). It is issued ONCE, after registration and
// BEFORE the first submit, then waited to idle: at that moment no frame is
// in flight, so the library's assembly workers have nothing to submit and
// the queue is not concurrently used.
bool UploadPattern(VkImageUsageFlags declaredUsage,
                   const DeviceFns& fns, VkPhysicalDevice phys, VkDevice device,
                   uint32_t queueFamily, VkImage image, VkFormat format,
                   std::string* err)
{
    std::vector<uint8_t> data; std::vector<PlaneCopy> planes;
    const size_t size = BuildPattern(format, &data, &planes);
    if (size == 0) { *err = "no pattern writer for this format"; return false; }

    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (fns.CreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS) { *err = "vkCreateBuffer failed"; return false; }
    VkMemoryRequirements req{}; fns.GetBufferMemoryRequirements(device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{}; fns.GetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t ti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (((req.memoryTypeBits & (1u << i)) != 0) && ((mp.memoryTypes[i].propertyFlags & want) == want)) { ti = i; break; }
    if (ti == UINT32_MAX) { fns.DestroyBuffer(device, buf, nullptr); *err = "no host-visible memory type"; return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = ti;
    if (fns.AllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) { fns.DestroyBuffer(device, buf, nullptr); *err = "vkAllocateMemory failed"; return false; }
    fns.BindBufferMemory(device, buf, mem, 0);
    void* mapped = nullptr;
    if (fns.MapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) { *err = "vkMapMemory failed"; return false; }
    std::memcpy(mapped, data.data(), size);
    fns.UnmapMemory(device, mem);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily;
    if (fns.CreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) { *err = "vkCreateCommandPool failed"; return false; }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (fns.AllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) { *err = "vkAllocateCommandBuffers failed"; return false; }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fns.BeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image; toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fns.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toDst);

    std::vector<VkBufferImageCopy> regions;
    for (const PlaneCopy& p : planes) {
        VkBufferImageCopy r{};
        r.bufferOffset = p.offset; r.bufferRowLength = 0; r.bufferImageHeight = 0;
        r.imageSubresource.aspectMask = p.aspect;
        r.imageSubresource.mipLevel = 0; r.imageSubresource.baseArrayLayer = 0; r.imageSubresource.layerCount = 1;
        r.imageOffset = {0, 0, 0}; r.imageExtent = {p.w, p.h, 1};
        regions.push_back(r);
    }
    fns.CmdCopyBufferToImage(cmd, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)regions.size(), regions.data());

    // To the layout the descriptor declares as defaultLayout -- GENERAL, or
    // TRANSFER_SRC_OPTIMAL under --declare-tso. The producer must actually
    // LEAVE the image where the registration says it is, or frame 1's acquire
    // names a layout the image was never in
    // (VUID-VkImageMemoryBarrier2-oldLayout-01197) and the mode would be
    // testing the test rather than the library.
    VkImageMemoryBarrier toGen{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGen.newLayout = DeclaredLayoutFor(declaredUsage);
    toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGen.image = image; toGen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    // VK_ACCESS_MEMORY_READ_BIT, not SHADER_READ|TRANSFER_READ, AND THAT IS A
    // FIX, NOT A TIDY-UP. This barrier is recorded on |encFamily| -- see the
    // sole UploadPattern call in RunRow, which passes the ENCODE family for
    // every row, DIRECT and FILTER alike. On this hardware family 4 reports
    // TRANSFER|SPARSE|VIDEO_ENCODE, so the expansion of
    // VK_PIPELINE_STAGE_ALL_COMMANDS_BIT below contains no stage that supports
    // VK_ACCESS_SHADER_READ_BIT, and the pair is invalid under
    // VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820.
    //
    // This is the whole of this suite's validation baseline: an uncorrected
    // line makes `--validate` report one "The Vulkan spec states" message per
    // session on the healthy default (RESIDENCY_LOCAL) run, all of them this
    // VUID. Nothing in the LIBRARY
    // was emitting them: the message names vkCmdPipelineBarrier (the v1 entry
    // point) and VkVideoEncoder::TransitionImageLayout records exclusively
    // through vkCmdPipelineBarrier2. It was this helper all along.
    //
    // WHY IT WAS INVISIBLE. This suite's stated bar is
    // `sessions=8 encoded=8 failures=0`, which carries no validation count, so
    // seven real messages sat under a green bar. The bar now includes
    // "0 spec-states under --validate"; see the file header.
    //
    // MEMORY_READ is a strict superset of the two flags it replaces and is
    // supported by every pipeline stage, so it is legal on every family this
    // helper could be asked to record on -- it does not merely move the
    // problem to whichever family a future row picks.
    toGen.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fns.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toGen);
    fns.EndCommandBuffer(cmd);

    VkQueue queue = VK_NULL_HANDLE;
    fns.GetDeviceQueue(device, queueFamily, 0, &queue);
    if (queue == VK_NULL_HANDLE) { *err = "vkGetDeviceQueue returned NULL"; return false; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    const VkResult sr = fns.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (sr != VK_SUCCESS) { *err = "vkQueueSubmit failed: " + std::to_string((int)sr); return false; }
    const VkResult wr = fns.QueueWaitIdle(queue);
    if (wr != VK_SUCCESS) { *err = "vkQueueWaitIdle failed: " + std::to_string((int)wr); return false; }

    fns.DestroyCommandPool(device, pool, nullptr);
    fns.DestroyBuffer(device, buf, nullptr);
    fns.FreeMemory(device, mem, nullptr);
    return true;
}

struct EncResult {
    bool     sessionInit   = false;
    // WHY the session failed, not just that it did. The skip decision below
    // turns on this: a device that is absent and a library that refused are
    // both "no session", and only the first is an environment condition.
    VkResult initResult    = VK_ERROR_INITIALIZATION_FAILED;
    bool     fileOpenFailed = false;
    bool     imageCreated  = false;
    bool     uploaded      = false;
    bool     registered    = false;
    bool     abandoned     = false;    // refused to submit (safety guard)
    std::string abandonReason;
    std::string uploadError;
    VkVideoEncoderStatusCode regStatus = VK_VIDEO_ENCODER_STATUS_ERROR_FORMAT_UNSUPPORTED;
    VkVideoEncoderExternalInputPath path = VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    VkBool32 planeStorageViews = VK_FALSE;
    VkBool32 storageReadView   = VK_FALSE;
    uint32_t submitted = 0, retrieved = 0;
    uint64_t bytes = 0;
    int      lastSubmitStatus = 0;
    bool     deviceLost = false;
    // The filter observable is read TWICE and both readings are reported. The
    // first run of this harness read it only after Flush() and got
    // filterCreated=0 on a session that had just encoded 12 frames -- the
    // documented "asked too late" answer. A single post-teardown reading is
    // therefore not evidence of anything, in either direction.
    // --content-probe. Read at TWO points for the same reason the filter
    // observable is: the registration echo says what was ARMED, and the
    // post-drain completion says what was SCORED. Only the pair can tell
    // "the detour never ran" from "the probe was never armed", and those are
    // different defects with different fixes.
    bool     contentChained = false;
    uint32_t contentArmEcho = 0;     // VkVideoEncoderImportContentState
    uint32_t contentArmGeneration = 0;
    uint32_t contentFinalState = 0;
    uint32_t contentProbedCount = 0;
    uint32_t contentDamagedCount = 0;
    uint32_t contentArmedCount = 0;
    uint32_t contentMeanY = 0, contentMeanU = 0, contentMeanV = 0;
    VkBool32 filterCreatedPre = VK_FALSE, filterCreatedPost = VK_FALSE;
    uint64_t filterDispatchPre = 0, stagedCopiesPre = 0;
    uint64_t filterDispatchPost = 0, stagedCopiesPost = 0;
    // --nv12-companion. Read BETWEEN the two submit loops, which is the whole
    // point: a single total cannot say which format dispatched.
    bool     companionAttempted = false;
    bool     companionRegistered = false;
    VkVideoEncoderStatusCode companionRegStatus = VK_VIDEO_ENCODER_STATUS_SUCCESS;
    VkVideoEncoderExternalInputPath companionPath = VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    uint32_t companionSubmitted = 0;
    std::string companionError;
    uint64_t dispatchAfterPrimary = 0, stagedAfterPrimary = 0;
    uint64_t dispatchAfterCompanion = 0, stagedAfterCompanion = 0;
    // --nv12-staged-companion. THE ANSWER THE MODE EXISTS FOR. Session-
    // constant, so one reading per session is the whole fact; read anyway at
    // both split points so a mid-session change could not hide.
    bool     stagedCompanion = false;
    VkVideoEncoderExternalInputPath companionExpectPath =
        VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
    uint32_t submitFlagsPrimary = 0, submitFamilyPrimary = VK_QUEUE_FAMILY_IGNORED;
    uint32_t submitFlagsCompanion = 0, submitFamilyCompanion = VK_QUEUE_FAMILY_IGNORED;
    uint64_t foreignAcqPrimary = 0, localAcqPrimary = 0;
    uint64_t foreignAcqCompanion = 0, localAcqCompanion = 0;
    std::string file;
};

// The staged-input queue answer, plus the two acquire counters, in one read.
// Chained the way the header documents -- CompletionInfo::pNext -- and read
// BEFORE Flush(), for the same reason ReadFilterInfo is.
void ReadResidencyInfo(VulkanVideoEncoderExt* enc, uint32_t* submitFlags,
                       uint32_t* submitFamily, uint64_t* foreignAcq,
                       uint64_t* localAcq)
{
    VkVideoEncoderStagedSubmitInfo si{};
    si.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STAGED_SUBMIT_INFO;
    VkVideoEncoderInputResidencyInfo ri{};
    ri.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_INPUT_RESIDENCY_INFO;
    ri.pNext = &si;   // two links on one chain, which the walk supports
    VkVideoEncoderCompletionInfo ci{};
    ci.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    ci.pNext = &ri;
    if (enc->GetCompletionInfo(&ci) == VK_SUCCESS) {
        *submitFlags  = si.submitTypeQueueFlags;
        *submitFamily = si.queueFamilyIndex;
        *foreignAcq   = ri.foreignAcquireCount;
        *localAcq     = ri.localAcquireCount;
    }
}

// VK_QUEUE_* bit -> the engine name, so the report does not make the reader
// decode a hex flag. Named from the flag rather than from the library's
// internal enum on purpose: the flag is what the submit used.
const char* SubmitEngineName(uint32_t queueFlags)
{
    switch (queueFlags) {
        case VK_QUEUE_GRAPHICS_BIT:            return "GRAPHICS";
        case VK_QUEUE_COMPUTE_BIT:             return "COMPUTE";
        case VK_QUEUE_TRANSFER_BIT:            return "TRANSFER";
        case VK_QUEUE_VIDEO_ENCODE_BIT_KHR:    return "VIDEO_ENCODE";
        case VK_QUEUE_VIDEO_DECODE_BIT_KHR:    return "VIDEO_DECODE";
        case 0:                                return "none/no-session";
        default:                               return "?";
    }
}

void ReadFilterInfo(VulkanVideoEncoderExt* enc, VkBool32* created,
                    uint64_t* dispatch, uint64_t* staged)
{
    VkVideoEncoderFilterInfo fi{};
    fi.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FILTER_INFO;
    VkVideoEncoderCompletionInfo ci{};
    ci.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_COMPLETION_INFO;
    ci.pNext = &fi;
    if (enc->GetCompletionInfo(&ci) == VK_SUCCESS) {
        *created = fi.filterCreated; *dispatch = fi.filterDispatchCount; *staged = fi.stagedCopyCount;
    }
}

EncResult RunRow(const Row& row, const char* outDir)
{
    EncResult res;
    VkSharedBaseObj<VulkanVideoEncoderExt> enc;
    if ((CreateVulkanVideoEncoderExt(enc) != VK_SUCCESS) || !enc) return res;

    const VkVideoComponentBitDepthFlagBitsKHR depth =
        (row.group == G_8BIT)  ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR :
        (row.group == G_10BIT) ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
                               : VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;

    VkVideoEncoderConfig config = {};
    config.sType           = VK_VIDEO_ENCODER_STRUCTURE_TYPE_CONFIG;
    config.codec           = row.codec;
    config.encodeWidth     = kWidth;  config.encodeHeight = kHeight;
    config.inputFormat     = row.format;
    config.inputWidth      = kWidth;  config.inputHeight  = kHeight;
    config.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    config.averageBitrate  = 20000000; config.maxBitrate = 20000000;
    config.gopLength = 30; config.consecutiveBFrames = 0; config.idrPeriod = 30;
    config.frameRateNum = 30; config.frameRateDen = 1;
    config.deviceId = -1;
    config.disableFileOutput = VK_TRUE;   // bitstream comes back in memory
    if (g_validate) { config.validate = VK_TRUE; }
    // THE FILTER-ENABLE DERIVATION, AND WHY IT IS NOT AN UNCONDITIONAL VK_TRUE
    // UNDER --nv12-staged-companion.
    //
    // This suite pins VK_TRUE on every row so that the routing gate, not the
    // flag, is what decides each row's path -- deliberate, and right for the
    // format matrix. It is WRONG for the staged-companion A/B, and the first
    // run of that mode caught it: with VK_TRUE pinned, the NV12-DECLARED row
    // also builds a filter object, so GetStagedInputSubmitType() answered
    // COMPUTE there too and the "control" measured the same value as the
    // subject. A control that cannot differ from its subject measures nothing.
    //
    // Chromium does NOT pin it. VulkanVideoEncoderConfigBuilder::Build sets
    //   enablePreprocessFilter =
    //       EncodesOnlyViaPreprocessFilter(inputFormat) ? VK_TRUE : VK_FALSE
    // so an NV12-declared session has NO filter and an RGBA- or I420-declared
    // one does. Reproducing that derivation is the whole content of the A/B:
    // the question is what DECLARING A WIDER SESSION FORMAT costs the NV12
    // frames riding on it, and that cost only exists if the narrow session is
    // measured without a filter.
    //
    // ARM_DIRECT rows are exactly the semi-planar (ENCODABLE_DIRECT) formats,
    // which is the complement of EncodesOnlyViaPreprocessFilter, so this
    // one-line mapping IS Chromium's predicate on this row set.
    config.enablePreprocessFilter =
        (g_stagedCompanion && row.arm == ARM_DIRECT) ? VK_FALSE : VK_TRUE;
    // Colour is PINNED, not defaulted. matrixCoefficients 1 = BT.709 and
    // videoFullRange FALSE = studio range are the same pair the YCbCr rows'
    // pattern is generated with, so the VUI the bitstream advertises and the
    // numbers actually written agree by construction. Leaving this at 0 makes
    // the library log "names no matrix ... converting as BT.709" and encode
    // BT.709 pixels under an unspecified VUI -- which is precisely the
    // contamination this matrix must not inherit.
    config.matrixCoefficients = 1;
    config.videoFullRange     = VK_FALSE;
    config.colourPrimaries    = 1;
    config.transferCharacteristics = 1;

    res.initResult = enc->InitializeExt(config);
    if (res.initResult != VK_SUCCESS) return res;
    res.sessionInit = true;

    VkInstance instance = enc->GetVkInstance();
    VkDevice   device   = enc->GetVkDevice();
    VkPhysicalDevice phys = enc->GetVkPhysicalDevice();
    DeviceFns fns;
    if (!LoadDeviceFns(instance, device, &fns)) return res;

    // The video-encode family: the one family this session certainly has a
    // queue on, and on this vendor it advertises TRANSFER.
    uint32_t qfCount = 0;
    fns.GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    fns.GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t encFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; i++)
        if ((qfs[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0 &&
            (qfs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) { encFamily = i; break; }
    if (encFamily == UINT32_MAX) { res.uploadError = "no encode family with TRANSFER"; return res; }

    const Decl d = DeclFor(row.arm);
    Img img;
    if (!CreateImage(fns, phys, device, row.format, d, row.codec, depth, &img)) return res;
    res.imageCreated = true;

    VkVideoEncoderExternalImageDescriptor desc{};
    desc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
    desc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
    desc.format = row.format; desc.width = kWidth; desc.height = kHeight;
    desc.tiling = d.tiling; desc.imageUsage = d.usage; desc.imageFlags = d.flags;
    desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE; desc.planeCount = 0;
    desc.residency = g_foreignResidency
                         ? VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN
                         : VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
    desc.defaultLayout = DeclaredLayoutFor(d.usage);
    desc.existingImage = img.image;

    VkVideoEncoderResource resource = VK_VIDEO_ENCODER_RESOURCE_NULL;
    // THE OPT-IN IS THE CHAIN ITSELF. There is no other switch: a library
    // whose caller never chains this struct never creates a probe object and
    // never takes the detour, which is what makes --content-probe a genuine
    // A/B against the default mode of this same binary.
    VkVideoEncoderImportContentInfo contentEcho = {};
    contentEcho.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO;
    VkVideoEncoderStatus regStatusStruct = {};
    regStatusStruct.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_STATUS;
    regStatusStruct.pNext = &contentEcho;
    res.regStatus = enc->RegisterImageResource(
        desc, 0, &resource, g_contentProbe ? &regStatusStruct : nullptr);
    if (g_contentProbe) {
        res.contentChained = true;
        res.contentArmEcho = (uint32_t)contentEcho.state;
        res.contentArmGeneration = contentEcho.probeGeneration;
    }
    if (res.regStatus != VK_VIDEO_ENCODER_STATUS_SUCCESS || resource == VK_VIDEO_ENCODER_RESOURCE_NULL) {
        DestroyImg(fns, device, &img); return res;
    }
    res.registered = true;
    VkEncResourceProbe probe;
    if (VkEncProbeResource(enc.get(), resource, &probe) == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
        res.path = probe.inputPath;
        res.planeStorageViews = probe.planeStorageViews;
        res.storageReadView   = probe.storageReadView;
    }

    // SAFETY GATE. A 3-plane input that reaches the staging copy is a
    // measured GPU hang, not a slow path. Abandon before any submit.
    if (row.arm == ARM_FILTER_YCBCR && res.path != VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER) {
        res.abandoned = true;
        res.abandonReason = std::string("3-plane input resolved to ") + PathName(res.path) +
                            ", not FILTER; staging a 3-plane input is a measured VK_ERROR_DEVICE_LOST. "
                            "No frame was submitted.";
        enc->UnregisterImageResource(resource);
        DestroyImg(fns, device, &img);
        return res;
    }

    std::string uerr;
    if (!UploadPattern(d.usage, fns, phys, device, encFamily, img.image, row.format, &uerr)) {
        res.uploadError = uerr;
        enc->UnregisterImageResource(resource);
        DestroyImg(fns, device, &img);
        return res;
    }
    res.uploaded = true;

    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.%s", outDir, row.shortName, row.ext);
    FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        // Leave res.file EMPTY. Every write below is guarded on |out|, so an
        // unwritable --out directory used to yield "RESULT: ENCODED -> path"
        // for nine files that do not exist -- and the colour verdict for this
        // suite is delegated to an out-of-process decoder reading exactly
        // those files, so the in-process half would claim success while the
        // out-of-process half had nothing to judge.
        res.fileOpenFailed = true;
    } else {
        res.file = path;
    }

    for (uint32_t i = 0; i < kFrames; i++) {
        VkVideoEncoderFrameSubmitInfo info{};
        info.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
        info.resource = resource;
        info.frameId = i; info.pts = i;
        info.forceIDR = (i == 0) ? VK_TRUE : VK_FALSE;
        info.isLastFrame = (i == kFrames - 1) ? VK_TRUE : VK_FALSE;
        info.qpOverride = -1;
        // UNDEFINED is the documented sentinel for "as declared at
        // registration", and under --declare-tso it is REQUIRED rather than
        // stylistic: a non-UNDEFINED per-frame layout is an explicit
        // statement that outranks the library's own residual record, which
        // would bypass the very mechanism the restore exists to feed.
        info.currentLayout = g_declareTso ? VK_IMAGE_LAYOUT_UNDEFINED
                                          : VK_IMAGE_LAYOUT_GENERAL;
        const VkVideoEncoderStatusCode s = enc->SubmitRegisteredFrame(info, nullptr);
        res.lastSubmitStatus = (int)s;
        if (s == VK_VIDEO_ENCODER_STATUS_SUCCESS) { res.submitted++; }
        else if (s == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
            // Capacity, not an error: drain and retry this same frame.
            VkVideoEncodeResult r{};
            while (enc->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
                res.retrieved++; res.bytes += r.bitstreamSize;
                if (out && r.pBitstreamData && r.bitstreamSize) std::fwrite(r.pBitstreamData, 1, r.bitstreamSize, out);
                enc->ReleaseEncodedFrame(r.frameId);
                r = VkVideoEncodeResult{};
            }
            i--; continue;
        } else { break; }

        VkVideoEncodeResult r{};
        while (enc->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
            res.retrieved++; res.bytes += r.bitstreamSize;
            if (out && r.pBitstreamData && r.bitstreamSize) std::fwrite(r.pBitstreamData, 1, r.bitstreamSize, out);
            enc->ReleaseEncodedFrame(r.frameId);
            r = VkVideoEncodeResult{};
        }
    }

    // Completion is ASYNCHRONOUS: a submit returns once the CPU-side pipeline
    // has issued, not once the bitstream exists. Acquiring in a single pass
    // therefore collects only whatever happened to be ready and silently
    // reports the rest as missing -- which on the first run of this harness
    // read as "12 submitted, 3 retrieved" and would have understated every
    // row. Poll until the retrieved count catches the submitted count, with a
    // bounded wait so a genuinely stuck frame still ends the run.
    // THE SPLIT READING. Taken before the companion submits anything, so
    // "did the RGBA frames dispatch" and "did the NV12 frames dispatch" are two
    // numbers rather than one sum. Without this the companion could not tell
    // per-frame routing from per-session routing, which is the only claim it is
    // here to test.
    {
        VkBool32 createdNow = VK_FALSE;
        ReadFilterInfo(enc.get(), &createdNow, &res.dispatchAfterPrimary,
                       &res.stagedAfterPrimary);
        ReadResidencyInfo(enc.get(), &res.submitFlagsPrimary,
                          &res.submitFamilyPrimary, &res.foreignAcqPrimary,
                          &res.localAcqPrimary);
    }

    // ---- --nv12-companion: an NV12 registration on this RGBA-declared session
    Img companion{};
    bool companionCreated = false;
    // ARM B is every RGBA-declared row. ARM A -- the control -- is the
    // 8-bit NV12-declared row, whose session has NO filter, and it is included
    // ONLY for the staged variant, because that is the only variant whose
    // answer differs between the two. The 10-bit DIRECT row is deliberately
    // excluded: its sessionEncodeFormat is P010, so an NV12 descriptor is a
    // format mismatch the registration correctly refuses, and a refusal there
    // would read as a failure of this mode rather than of nothing.
    // AV1 ROWS ARE EXCLUDED FROM BOTH COMPANION MODES, DELIBERATELY. Those
    // two modes are A/Bs about a Chromium-shaped question -- what declaring a
    // wider session input format costs the shipping NV12 lane -- and their
    // measured bars are stated in H.26x terms. Their answer is decided by
    // whether the session carries a filter OBJECT, which is a function of the
    // declared input format and not of the codec, so admitting AV1 rows would
    // move those numbers without adding an observation. Excluding them keeps
    // every existing companion measurement byte-identical.
    const bool companionCodecEligible =
        (row.codec != VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR);
    const bool companionOnThisRow =
        companionCodecEligible &&
        ((g_nv12Companion && row.arm == ARM_FILTER_RGBA) ||
         (g_stagedCompanion &&
          ((row.arm == ARM_FILTER_RGBA) ||
           (row.arm == ARM_DIRECT && row.group == G_8BIT))));
    if (companionOnThisRow) {
        res.companionAttempted = true;
        res.stagedCompanion = g_stagedCompanion;
        res.companionExpectPath = g_stagedCompanion
                                      ? VK_VIDEO_EXTERNAL_INPUT_PATH_STAGED
                                      : VK_VIDEO_EXTERNAL_INPUT_PATH_DIRECT;
        const Decl cd = g_stagedCompanion ? StagedCompanionDecl()
                                          : DeclFor(ARM_DIRECT);
        if (!CreateImage(fns, phys, device, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                         cd, row.codec, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
                         &companion)) {
            res.companionError = "NV12 companion vkCreateImage failed";
        } else {
            companionCreated = true;
            VkVideoEncoderExternalImageDescriptor cdesc{};
            cdesc.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_EXTERNAL_IMAGE_DESCRIPTOR;
            cdesc.handleType = VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_VK_IMAGE;
            cdesc.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
            cdesc.width = kWidth; cdesc.height = kHeight;
            cdesc.tiling = cd.tiling; cdesc.imageUsage = cd.usage;
            cdesc.imageFlags = cd.flags;
            cdesc.sharingMode = VK_SHARING_MODE_EXCLUSIVE; cdesc.planeCount = 0;
            cdesc.residency = (g_foreignResidency || g_companionForeign)
                                  ? VK_VIDEO_ENCODER_INPUT_RESIDENCY_FOREIGN
                                  : VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL;
            cdesc.defaultLayout = DeclaredLayoutFor(cd.usage);
            cdesc.existingImage = companion.image;

            VkVideoEncoderResource cres = VK_VIDEO_ENCODER_RESOURCE_NULL;
            res.companionRegStatus =
                enc->RegisterImageResource(cdesc, 0, &cres, nullptr);
            if (res.companionRegStatus == VK_VIDEO_ENCODER_STATUS_SUCCESS &&
                cres != VK_VIDEO_ENCODER_RESOURCE_NULL) {
                res.companionRegistered = true;
                VkEncResourceProbe cprobe;
                if (VkEncProbeResource(enc.get(), cres, &cprobe) ==
                    VK_VIDEO_ENCODER_STATUS_SUCCESS) {
                    res.companionPath = cprobe.inputPath;
                }
                std::string cerr;
                if (!UploadPattern(cd.usage, fns, phys, device, encFamily,
                                   companion.image,
                                   VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &cerr)) {
                    res.companionError = "NV12 companion upload: " + cerr;
                } else {
                    for (uint32_t i = 0; i < kFrames; i++) {
                        VkVideoEncoderFrameSubmitInfo ci{};
                        ci.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_FRAME_PARAMS;
                        ci.resource = cres;
                        ci.frameId = kFrames + i; ci.pts = kFrames + i;
                        ci.forceIDR = VK_FALSE;
                        ci.isLastFrame = (i == kFrames - 1) ? VK_TRUE : VK_FALSE;
                        ci.qpOverride = -1;
                        ci.currentLayout = g_declareTso
                                               ? VK_IMAGE_LAYOUT_UNDEFINED
                                               : VK_IMAGE_LAYOUT_GENERAL;
                        const VkVideoEncoderStatusCode cs =
                            enc->SubmitRegisteredFrame(ci, nullptr);
                        if (cs == VK_VIDEO_ENCODER_STATUS_SUCCESS) {
                            res.companionSubmitted++;
                        } else if (cs == VK_VIDEO_ENCODER_STATUS_NOT_READY) {
                            VkVideoEncodeResult r{};
                            while (enc->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
                                res.retrieved++; res.bytes += r.bitstreamSize;
                                if (out && r.pBitstreamData && r.bitstreamSize)
                                    std::fwrite(r.pBitstreamData, 1, r.bitstreamSize, out);
                                enc->ReleaseEncodedFrame(r.frameId);
                                r = VkVideoEncodeResult{};
                            }
                            i--; continue;
                        } else {
                            res.companionError =
                                "NV12 companion submit status " + std::to_string((int)cs);
                            break;
                        }
                        VkVideoEncodeResult r{};
                        while (enc->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
                            res.retrieved++; res.bytes += r.bitstreamSize;
                            if (out && r.pBitstreamData && r.bitstreamSize)
                                std::fwrite(r.pBitstreamData, 1, r.bitstreamSize, out);
                            enc->ReleaseEncodedFrame(r.frameId);
                            r = VkVideoEncodeResult{};
                        }
                    }
                }
                VkBool32 createdNow = VK_FALSE;
                ReadFilterInfo(enc.get(), &createdNow,
                               &res.dispatchAfterCompanion,
                               &res.stagedAfterCompanion);
                ReadResidencyInfo(enc.get(), &res.submitFlagsCompanion,
                                  &res.submitFamilyCompanion,
                                  &res.foreignAcqCompanion,
                                  &res.localAcqCompanion);
                enc->UnregisterImageResource(cres);
            } else {
                res.companionError = "NV12 companion registration refused";
            }
        }
        // The submitted total must include the companion so the retrieved/
        // submitted balance check below still means what it says.
        res.submitted += res.companionSubmitted;
    }

    // BEFORE Flush: dispatches are RECORDED inline on the submit call, so the
    // full count already exists here.
    ReadFilterInfo(enc.get(), &res.filterCreatedPre, &res.filterDispatchPre, &res.stagedCopiesPre);

    if (enc->Flush() == VK_ERROR_DEVICE_LOST) res.deviceLost = true;
    for (int spin = 0; spin < 2000 && res.retrieved < res.submitted; spin++) {
        VkVideoEncodeResult r{};
        while (enc->AcquireNextEncodedFrame(r) == VK_SUCCESS) {
            res.retrieved++; res.bytes += r.bitstreamSize;
            if (out && r.pBitstreamData && r.bitstreamSize) std::fwrite(r.pBitstreamData, 1, r.bitstreamSize, out);
            enc->ReleaseEncodedFrame(r.frameId);
            r = VkVideoEncodeResult{};
        }
        if (res.retrieved < res.submitted) usleep(5000);
    }
    if (out) std::fclose(out);

    ReadFilterInfo(enc.get(), &res.filterCreatedPost, &res.filterDispatchPost, &res.stagedCopiesPost);

    // READ BEFORE THE RETIREMENT, and that ordering is load-bearing:
    // UnregisterImageResource calls ForgetRegistration, which erases the
    // verdict and drops the outstanding count. Reading after it would report
    // a clean, empty probe for a session that had just found damage.
    if (g_contentProbe) {
        VkVideoEncoderImportContentInfo verdict = {};
        verdict.sType = VK_VIDEO_ENCODER_STRUCTURE_TYPE_IMPORT_CONTENT_INFO;
        VkVideoEncoderCompletionInfo completion = {};
        completion.pNext = &verdict;
        enc->GetCompletionInfo(&completion);
        res.contentFinalState   = (uint32_t)verdict.state;
        res.contentProbedCount  = verdict.probedRegistrationCount;
        res.contentDamagedCount = verdict.damagedRegistrationCount;
        res.contentArmedCount   = verdict.armedRegistrationCount;
        res.contentMeanY        = verdict.meanY;
        res.contentMeanU        = verdict.meanU;
        res.contentMeanV        = verdict.meanV;
    }

    enc->UnregisterImageResource(resource);
    DestroyImg(fns, device, &img);
    if (companionCreated) { DestroyImg(fns, device, &companion); }
    enc = nullptr;
    return res;
}


// ---------------------------------------------------------------------------
// THE FOUR-QUADRANT DECODE ASSERTION.
//
// WHY IT EXISTS. This file's header has promised since it was written that
// "Colour is judged OUT OF PROCESS from the written file". Nothing ever ran
// that judgement: the suite wrote seven elementary streams and no line of code
// -- here or in CMakeLists.txt -- ever looked at a pixel in them. It was a
// documented acceptance criterion with nothing behind it.
//
// THE GAP IS NOT THEORETICAL. Commenting out the four
// m_vkDevCtx->CmdDispatch calls in
// common/libs/VkCodecUtils/VulkanFilterYuvCompute.cpp DESTROYS the decoded
// picture -- every quadrant of frame 0 of all five FILTER-routed rows becomes
// a flat (0,76,0), which is BT.709-limited (16,128,128), i.e. the zero-filled
// output image the filter never wrote to -- while EVERY OTHER OBSERVABLE IN
// THIS SUITE STAYS GREEN. Every session still reports encoded, every filter
// row still reports a dispatch, no staged copy is taken, no device is lost,
// every submitted frame is retrieved and the byte counts do not move.
//
// The dispatch counter cannot see it because it counts command buffers
// RECORDED, not executed: VkVideoEncoder.cpp increments it immediately after
// RecordCommandBuffer returns VK_SUCCESS, and a command buffer with the
// vkCmdDispatch deleted records just as successfully as one without. The
// decoded picture is the only observable in this suite that can tell the
// difference, which is why this runs in the DEFAULT verdict chain and not
// behind an opt-in flag.
//
// WHY A TOLERANCE AND NOT A CHECKSUM. An md5 pin over the bitstream was
// considered and rejected: it goes red on any benign encoder change, it gets
// disabled the first time it does, and then the suite is back where it
// started with a colour criterion nothing enforces.
//
// THE TOLERANCE, AND THE ARITHMETIC BEHIND IT. Every figure below is an
// L-infinity (worst single channel) deviation, in 8-bit RGB, of a decoded
// quadrant centre from the RGB the pattern writer put in that quadrant.
// The figures are:
//
//   HEALTHY tree, all 7 encoding rows x 4 quadrants x 3 channels:  worst = 2
//     (the 8-bit rows land TL(251,0,0) TR(0,251,0) BL(0,0,252) BR(255,255,255)
//      against a written 253/253/252/255; the 10-bit rows land TL(253,0,0)
//      TR(0,252,1) BL(0,0,253) BR(255,253,255).)
//   HEALTHY tree, spatial spread within a 41x41 box centred on each sample
//     point: 0 on every row, quadrant and channel. The decoded quadrants are
//     exactly flat, so the single-pixel sample below is a measurement and not
//     a lucky draw.
//   DEAD FILTER (the flat 0,76,0 above): TL 253, TR 177, BL 252, BR 255. The
//     SMALLEST of those -- the number the tolerance actually has to sit under
//     -- is 177.
//   RED/BLUE CHANNEL SWAP in the RGBA storage-read arm: >= 251 on TL and BL.
//     TR (green) and BR (white) are invariant under that swap, which is
//     exactly why the assertion is per-quadrant and not an aggregate.
//
// kQuadTolerance = 24 is 12x the largest deviation a healthy encode has ever
// produced here, and 7.4x below the smallest deviation any failure mode in
// scope produces. Rate-control drift, rounding changes and swscale version
// differences live in the first gap; a dead filter and a colour swap live in
// the second. There is no legitimate encoder change that moves a flat primary
// by 24/255 and is still the same picture.
//
// EXPECTED VALUES COME FROM QuadAt(), NOT FROM A TABLE OF DECODED NUMBERS.
// Tying the assertion to the pattern the uploader actually writes means the
// two cannot drift apart, and it means the geometry -- which quadrant is
// where -- is stated once in this file rather than twice.
//
// ---- WHAT THE OLD NUMBER WAS, and why its arithmetic is no longer quoted --
//
// kQuadTolerance was 24, described as "12x the largest deviation a healthy
// encode has ever produced here, and 7.4x below the smallest deviation any
// failure mode in scope produces". Both multipliers were true OF THE RGB
// DOMAIN and of the failure set considered then -- a dead filter (smallest
// deviation 177) and a red/blue channel swap (>= 251). They were never
// measured against a MATRIX confusion, and the sentence "there is no
// legitimate encoder change that moves a flat primary by 24/255 and is still
// the same picture" is true and beside the point: a matrix confusion moves it
// by 23, and 23 < 24.
//
// The AV1 paragraph that followed is also superseded, and its finding is
// preserved because it still holds: measured on the A4000 host with the SAME
// pattern writer at 1920x1080, H.264 (libx264), AV1 (libsvtav1, raw OBU) and
// AV1 (libsvtav1, real IVF) all produced worst channel delta 2 with 41x41
// spread 0 -- identical to the count. The quadrants decode exactly flat on
// AV1 as they do on H.26x, so the single-pixel sample is as sound on AV1 as
// on H.26x. That bounded the CODEC and the colour conversion, not the rate
// control of the Vulkan AV1 encoder; if a Blackwell run shows AV1 quadrant
// deltas materially above the healthy figure, the correct response is still a
// separate AV1-specific constant here rather than a wider shared tolerance.
const int kQuadTolerance = 4;

// THE DECODER INVOCATION, PINNED. Frame 0 only, chroma un-subsampled to
// yuv444p on stdout:
//
//   ffmpeg -v error -i <file> -frames:v 1 -pix_fmt yuv444p -f rawvideo -
//
// NO COLOUR CONVERSION AT ALL. yuv444p is the decoder's own output format
// with the chroma planes upsampled; there is no inverse matrix, no gamut
// clamp and no dependence on the stream's VUI. That last part is why
// CheckColourLabel() exists -- the old rgb24 invocation checked the label
// implicitly by consulting the VUI, and this one cannot.
enum QuadStatus {
    QUAD_PASS,
    QUAD_MISMATCH,       // the decoder ran and the picture is wrong -- FAILURE
    QUAD_DECODE_FAILED,  // the decoder is present and produced no frame -- FAILURE
    QUAD_NO_DECODER,     // there is no ffmpeg at all -- a SKIP, and a loud one
};

int  g_quadChecked   = 0;
int  g_quadFailed    = 0;
bool g_quadNoDecoder = false;

// Probed ONCE per run. -1 unknown, 0 absent, 1 present.
int         g_ffmpegProbe = -1;
std::string g_ffmpegBanner;

void DrainToEof(FILE* p)
{
    char scratch[65536];
    while (std::fread(scratch, 1, sizeof(scratch), p) != 0) { }
}

bool HaveDecoder()
{
    if (g_ffmpegProbe < 0) {
        g_ffmpegProbe = 0;
        FILE* p = popen("ffmpeg -version 2>/dev/null", "r");
        if (p != nullptr) {
            char line[256];
            line[0] = '\0';
            const bool got = (std::fgets(line, sizeof(line), p) != nullptr);
            DrainToEof(p);
            const int rc = pclose(p);
            if (rc == 0 && got) {
                g_ffmpegProbe = 1;
                std::string b(line);
                while (!b.empty() && (b[b.size() - 1] == '\n' || b[b.size() - 1] == '\r')) {
                    b.erase(b.size() - 1);
                }
                g_ffmpegBanner = b;
            }
        }
    }
    return g_ffmpegProbe == 1;
}

int AbsDiff(int a, int b) { return (a > b) ? (a - b) : (b - a); }

struct QuadPoint { const char* name; uint32_t x, y; };

QuadStatus CheckQuadrants(const std::string& file, std::string* detail)
{
    if (!HaveDecoder()) {
        *detail = "ffmpeg is not on PATH";
        return QUAD_NO_DECODER;
    }
    // --out is caller-controlled and the path is interpolated into a shell
    // command. A single quote in it would let the argument rewrite the command
    // rather than name a file; refuse instead of running something else.
    if (file.find('\'') != std::string::npos) {
        *detail = "the output path contains a single quote and cannot be handed to the decoder";
        return QUAD_DECODE_FAILED;
    }

    char cmd[1024];
    const int need = std::snprintf(
        cmd, sizeof(cmd),
        "ffmpeg -v error -i '%s' -frames:v 1 -pix_fmt rgb24 -f rawvideo -",
        file.c_str());
    // A silently truncated command line would name a DIFFERENT file, and the
    // decode would fail for a reason that has nothing to do with the picture.
    // A gate that can go red for a reason it does not name is a gate that gets
    // switched off.
    if (need < 0 || (size_t)need >= sizeof(cmd)) {
        *detail = "the decoder command line does not fit; --out path is too long";
        return QUAD_DECODE_FAILED;
    }
    FILE* p = popen(cmd, "r");
    if (p == nullptr) {
        *detail = "popen(ffmpeg) failed";
        return QUAD_DECODE_FAILED;
    }

    std::vector<uint8_t> rgb((size_t)kWidth * (size_t)kHeight * 3);
    size_t got = 0;
    while (got < rgb.size()) {
        const size_t n = std::fread(rgb.data() + got, 1, rgb.size() - got, p);
        if (n == 0) break;
        got += n;
    }
    DrainToEof(p);
    const int rc = pclose(p);
    if (rc != 0 || got != rgb.size()) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "the decoder produced %zu of %zu bytes for frame 0 "
                      "(ffmpeg exit status %d). A file this suite reported as "
                      "ENCODED that ffmpeg cannot decode is a failure, not a skip",
                      got, rgb.size(), rc);
        *detail = msg;
        return QUAD_DECODE_FAILED;
    }

    const QuadPoint pts[4] = {
        {"TL", kWidth / 4,       kHeight / 4},
        {"TR", 3 * kWidth / 4,   kHeight / 4},
        {"BL", kWidth / 4,       3 * kHeight / 4},
        {"BR", 3 * kWidth / 4,   3 * kHeight / 4},
    };

    std::string seen;
    std::string bad;
    int worst = 0;
    int nBad  = 0;
    for (int q = 0; q < 4; q++) {
        const size_t o = ((size_t)pts[q].y * (size_t)kWidth + (size_t)pts[q].x) * 3;
        const int gr = (int)rgb[o], gg = (int)rgb[o + 1], gb = (int)rgb[o + 2];
        const RGB want = QuadAt(pts[q].x, pts[q].y);
        const int dr = AbsDiff(gr, (int)want.r);
        const int dg = AbsDiff(gg, (int)want.g);
        const int db = AbsDiff(gb, (int)want.b);
        const int dmax = (dr > dg) ? ((dr > db) ? dr : db) : ((dg > db) ? dg : db);
        if (dmax > worst) worst = dmax;

        char one[64];
        std::snprintf(one, sizeof(one), " %s(%d,%d,%d)", pts[q].name, gr, gg, gb);
        seen += one;

        if (dmax > kQuadTolerance) {
            nBad++;
            // NAME THE QUADRANT AND THE AMOUNT. A gate that says only "colour
            // wrong" is undebuggable from a CI log, and which quadrants moved
            // is the diagnosis: all four flat means the filter is not running,
            // TL and BL swapped with TR and BR intact means red and blue are
            // crossed.
            const char* ch = (dmax == dr) ? "R" : ((dmax == dg) ? "G" : "B");
            char m[320];
            std::snprintf(m, sizeof(m),
                          "\n        %s centre (%u,%u): got (%d,%d,%d) want (%d,%d,%d) "
                          "delta (%d,%d,%d) -- worst channel %s off by %d, tolerance %d",
                          pts[q].name, pts[q].x, pts[q].y, gr, gg, gb,
                          (int)want.r, (int)want.g, (int)want.b, dr, dg, db,
                          ch, dmax, kQuadTolerance);
            bad += m;
        }
    }

    if (nBad != 0) {
        char head[160];
        std::snprintf(head, sizeof(head),
                      "%d of 4 quadrant centres of frame 0 are outside +/-%d",
                      nBad, kQuadTolerance);
        *detail = std::string(head) + bad;
        return QUAD_MISMATCH;
    }

    char ok[320];
    std::snprintf(ok, sizeof(ok), "%s  worst channel delta %d (tolerance %d)",
                  seen.c_str(), worst, kQuadTolerance);
    *detail = ok;
    return QUAD_PASS;
}

}  // namespace

int main(int argc, char** argv)
{
    const char* outDir = ".";
    const char* only = nullptr;
    // --codec. A CODEC-WIDE row filter, which --only cannot express: --only
    // takes ONE shortName and the AV1 arm is four rows. It exists so ctest
    // can register the AV1 arm as its own entry -- see CMakeLists.txt, which
    // also states how that entry stays GREEN (SKIPPED, 77) on hardware
    // without AV1 rather than reddening the gating set.
    const char* onlyCodec = nullptr;
    std::vector<VkResult> rowResults;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outDir = argv[++i];
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
        else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) onlyCodec = argv[++i];
        else if (std::strcmp(argv[i], "--rawbits") == 0) g_rawBits = true;
        else if (std::strcmp(argv[i], "--declare-tso") == 0) g_declareTso = true;
        else if (std::strcmp(argv[i], "--content-probe") == 0) g_contentProbe = true;
        else if (std::strcmp(argv[i], "--validate") == 0) g_validate = true;
        else if (std::strcmp(argv[i], "--foreign-residency") == 0) g_foreignResidency = true;
        else if (std::strcmp(argv[i], "--nv12-companion") == 0) g_nv12Companion = true;
        else if (std::strcmp(argv[i], "--nv12-staged-companion") == 0) g_stagedCompanion = true;
        else if (std::strcmp(argv[i], "--companion-foreign") == 0) g_companionForeign = true;
    }

    std::printf("Encoder-ext INPUT FORMAT *ENCODE* MATRIX -- library-owned device\n");
    std::printf("================================================================\n");
    std::printf("pattern: TL(253,0,0) TR(0,253,0) BL(0,0,252) BR(255,255,255)\n");
    std::printf("colour : matrixCoefficients=1 (BT.709), videoFullRange=FALSE (studio)\n");
    std::printf("frames : %u per format\n", kFrames);
    if (g_stagedCompanion) {
        std::printf("companion: LINEAR/TRANSFER_SRC NV12 descriptor + %u NV12 frames,\n"
                    "           on the NV12-declared row (arm A, no filter) AND on each\n"
                    "           RGBA-declared row (arm B, filter present). Reports the\n"
                    "           staged-input SUBMIT ENGINE for each -- the queue-family\n"
                    "           move this widening causes on the shipping NV12 lane.\n",
                    kFrames);
    }
    if (g_nv12Companion) {
        std::printf("companion: NV12 descriptor + %u NV12 frames on each "
                    "RGBA-DECLARED session (the shape Chromium now builds)\n",
                    kFrames);
    }
    std::printf("declared input layout: %s\n\n",
                g_declareTso ? "TRANSFER_SRC_OPTIMAL where usage permits "
                               "(exercises the filter-arm restore)"
                             : "GENERAL");
    std::printf("declared residency   : %s\n\n",
                g_foreignResidency
                    ? "FOREIGN (drives the filter-arm and Path A releases)"
                    : "LOCAL");

    size_t nSession = 0, nEncoded = 0, nAbandoned = 0;
    // Rows the DEVICE refused, kept apart from failures because they are not
    // one -- and apart from silence, because a new arm that is only ever
    // silent is a new arm that never ran. See IsDeviceLimitedInit().
    size_t nDeviceLimited = 0, nAv1Unverified = 0, nSelected = 0;
    for (size_t i = 0; i < kNumRows; i++) {
        const Row& row = kRows[i];
        if (only && std::strcmp(only, row.shortName) != 0) continue;
        if (onlyCodec && std::strcmp(onlyCodec, CodecName(row.codec)) != 0) continue;
        nSelected++;
        std::printf("[%zu/%zu] %s\n", i + 1, kNumRows, row.name);
        std::fflush(stdout);

        const EncResult r = RunRow(row, outDir);

        if (!r.sessionInit) {
            rowResults.push_back(r.initResult);
            const bool noDriver = (r.initResult == VK_ERROR_INCOMPATIBLE_DRIVER);
            const bool deviceLimited = IsDeviceLimitedInit(r.initResult);

            // THE AV1 ARM'S VERDICT, which is the half of this change that is
            // worth more than the rows themselves. "No session" is free; it
            // is what a new codec arm reports on the day it is added and on
            // every day after, whether or not it works. So for AV1 the claim
            // is put to the device -- see ProbeAv1EncodeSupport().
            if ((row.codec == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR) &&
                !noDriver) {
                ProbeAv1EncodeSupport();
                if (g_av1Probe == 1) {
                    g_failures++;
                    std::printf("      RESULT: FAIL -- no session "
                                "(InitializeExt -> %d), but the device DOES "
                                "advertise AV1 encode [%s]. That is a refusal, "
                                "not a device limitation, and calling it one "
                                "is how this arm would pass without ever "
                                "encoding a frame.\n\n",
                                (int)r.initResult, g_av1ProbeDetail.c_str());
                } else if (g_av1Probe == 0) {
                    if (deviceLimited) {
                        nDeviceLimited++;
                        std::printf("      RESULT: DEVICE-LIMITED -- this "
                                    "hardware has no AV1 encode "
                                    "(InitializeExt -> %d; %s). Not a "
                                    "failure.\n\n",
                                    (int)r.initResult, g_av1ProbeDetail.c_str());
                    } else {
                        g_failures++;
                        std::printf("      RESULT: FAIL -- no session "
                                    "(InitializeExt -> %d). The device does "
                                    "not advertise AV1 encode, but %d is not "
                                    "a device-limitation code either: this is "
                                    "a library refusal wearing an environment "
                                    "costume.\n\n",
                                    (int)r.initResult, (int)r.initResult);
                    }
                } else {
                    nAv1Unverified++;
                    std::printf("      RESULT: UNVERIFIED -- no session "
                                "(InitializeExt -> %d) and the AV1 capability "
                                "probe could not run (%s), so whether this is "
                                "a device limitation is UNJUDGED.\n\n",
                                (int)r.initResult, g_av1ProbeDetail.c_str());
                }
                continue;
            }

            // The nine H.26x rows KEEP the verdict they have always had: no
            // session is reported and is not a failure. What is new is that
            // the classification is PRINTED, which it was not -- a library
            // refusal here is otherwise indistinguishable in the log from an absent
            // profile.
            //
            // NOT TIGHTENED, and that is scoped rather than overlooked.
            // Turning a non-device-limited no-session into a failure for
            // those nine would change the verdict of a suite in the CI gating
            // set on a prediction this change cannot measure -- the reference
            // host is not available to this work, and a red suite on it
            // blocks everyone. The residual is real and is one measured A4000
            // run away from closing exactly the way the AV1 arm above already
            // has.
            std::printf("      RESULT: no session (InitializeExt -> %d)%s\n\n",
                        (int)r.initResult,
                        noDriver ? " -- no usable driver"
                                 : (deviceLimited
                                        ? " -- DEVICE-LIMITED"
                                        : " -- NOT a device-limitation code, "
                                          "i.e. a library refusal"));
            if (deviceLimited) nDeviceLimited++;
            continue;
        }
        nSession++;
        std::printf("      registered=%d status=%d  ROUTED=%s planeStorage=%d storageRead=%d\n",
                    (int)r.registered, (int)r.regStatus, PathName(r.path),
                    (int)r.planeStorageViews, (int)r.storageReadView);
        if (r.abandoned) {
            nAbandoned++;
            std::printf("      ABANDONED (safety): %s\n\n", r.abandonReason.c_str());
            continue;
        }
        if (!r.uploaded) {
            std::printf("      RESULT: upload failed: %s\n\n", r.uploadError.c_str());
            g_failures++;
            continue;
        }
        std::printf("      submitted=%u retrieved=%u bytes=%llu lastSubmitStatus=%d deviceLost=%d\n",
                    r.submitted, r.retrieved, (unsigned long long)r.bytes,
                    r.lastSubmitStatus, (int)r.deviceLost);
        std::printf("      filter PRE-flush : created=%d dispatch=%llu stagedCopies=%llu\n",
                    (int)r.filterCreatedPre, (unsigned long long)r.filterDispatchPre,
                    (unsigned long long)r.stagedCopiesPre);
        std::printf("      filter POST-flush: created=%d dispatch=%llu stagedCopies=%llu\n",
                    (int)r.filterCreatedPost, (unsigned long long)r.filterDispatchPost,
                    (unsigned long long)r.stagedCopiesPost);
        if (r.contentChained) {
            std::printf("      CONTENT PROBE: armEcho=%s(gen=%u) final=%s "
                        "probed=%u damaged=%u armedOutstanding=%u "
                        "meanY=%u meanU=%u meanV=%u (Q8)\n",
                        ContentStateName(r.contentArmEcho),
                        r.contentArmGeneration,
                        ContentStateName(r.contentFinalState),
                        r.contentProbedCount, r.contentDamagedCount,
                        r.contentArmedCount,
                        r.contentMeanY, r.contentMeanU, r.contentMeanV);
            // THE VERDICT, and it is scoped to the rows that can carry it.
            //
            // A DIRECT row is the whole point: it is block-linear plus
            // VIDEO_ENCODE_SRC, so it is the class D2 unblinded, and it is
            // the ONLY class that needs the staged detour to be probed at
            // all. A STAGED row already passes through the capture site
            // without any detour, so it proves the plumbing but not the
            // detour. A FILTER row is a storage read with no copy, so
            // NOT_APPLICABLE is its correct answer and anything else is the
            // arm predicate having regressed into "arm everything".
            // WHAT THIS ROW SHOULD ANSWER, computed from the same two facts
            // the library's arm predicate reads -- deliberately restated here
            // rather than imported, so a change to the predicate has to be
            // made twice and cannot silently redefine its own test.
            //
            //   FILTER route  -> a storage read, never a copy, so there is no
            //                    capture site at all.
            //   not 8-bit 420 -> the scorer reads plane means byte-wise and
            //                    would read the wrong half of a 10/12-bit
            //                    word, so there are no bytes it can judge.
            //
            // The P010 row is the reason this is here. Before the format
            // clause was added to the arm predicate it echoed ARMED and then
            // scored nothing, and this suite reported that as a failure --
            // which is how the defect was found.
            const bool probeScorableFormat =
                (row.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
            const bool expectArm =
                (r.path != VK_VIDEO_EXTERNAL_INPUT_PATH_FILTER) &&
                probeScorableFormat;
            if (!expectArm) {
                if (r.contentArmEcho !=
                    VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_NOT_APPLICABLE) {
                    g_failures++;
                    std::printf("      VERDICT: FAIL -- a registration the "
                                "probe cannot ride (%s route, format %s) "
                                "echoed %s, not NOT_APPLICABLE. An ARMED echo "
                                "is a PROMISE of a verdict; promising one "
                                "here leaves the registration ARMED forever "
                                "reporting NOT_EVALUATED, which is an "
                                "observable that cannot fail.\n",
                                PathName(r.path),
                                probeScorableFormat ? "scorable"
                                                    : "not 8-bit 420",
                                ContentStateName(r.contentArmEcho));
                } else {
                    std::printf("      VERDICT: content probe correctly "
                                "REFUSED this registration up front (%s "
                                "route, format %s)\n",
                                PathName(r.path),
                                probeScorableFormat ? "scorable"
                                                    : "not 8-bit 420");
                }
            } else if (r.contentArmEcho !=
                       VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_ARMED) {
                g_failures++;
                std::printf("      VERDICT: FAIL -- a %s registration echoed "
                            "%s, not ARMED. The arm predicate is reading "
                            "something other than 'is a transfer-readable "
                            "copy of the producer's pixels reachable'.\n",
                            PathName(r.path),
                            ContentStateName(r.contentArmEcho));
            } else if (r.contentProbedCount == 0) {
                // THE ONE THIS MODE EXISTS FOR.
                g_failures++;
                std::printf("      VERDICT: FAIL -- the registration ARMED "
                            "and NOTHING WAS EVER SCORED (probed=0, "
                            "armedOutstanding=%u) after %u submitted and %u "
                            "retrieved frames. On a %s row that means the "
                            "one-frame staged detour did not fire or the "
                            "capture site was not reached. This is the exact "
                            "state the device-free suite reports as a PASS.\n",
                            r.contentArmedCount, r.submitted, r.retrieved,
                            PathName(r.path));
            } else if (r.contentArmedCount != 0) {
                g_failures++;
                std::printf("      VERDICT: FAIL -- the session ended with %u "
                            "registration(s) still armed and unscored.\n",
                            r.contentArmedCount);
            } else if (r.contentFinalState !=
                       VK_VIDEO_ENCODER_IMPORT_CONTENT_STATE_CLEAN) {
                // NOT automatically a library bug -- DAMAGED_* here would be
                // the driver defect firing on a locally-allocated VkImage,
                // which has never been observed (the defect is GBM-import
                // gated). Either way it is not the expected result of
                // encoding a four-quadrant colour bar, so it is reported
                // loudly rather than absorbed.
                g_failures++;
                std::printf("      VERDICT: FAIL -- scored %s on an image the "
                            "harness filled with a four-quadrant colour bar. "
                            "A colour bar has no dead plane, so either the "
                            "capture read the wrong image or the predicate "
                            "regressed.\n",
                            ContentStateName(r.contentFinalState));
            } else {
                std::printf("      VERDICT: content probe OK -- ARMED at "
                            "registration, and the capture RAN and scored "
                            "CLEAN on a %s row (probed=%u, nothing left "
                            "outstanding)\n",
                            PathName(r.path), r.contentProbedCount);
            }
        }
        if (r.companionAttempted) {
            std::printf("      NV12 COMPANION on this RGBA-declared session:\n");
            std::printf("        registered=%d status=%d ROUTED=%s submitted=%u\n",
                        (int)r.companionRegistered, (int)r.companionRegStatus,
                        PathName(r.companionPath), r.companionSubmitted);
            std::printf("        dispatch after RGBA frames  = %llu (stagedCopies=%llu)\n",
                        (unsigned long long)r.dispatchAfterPrimary,
                        (unsigned long long)r.stagedAfterPrimary);
            std::printf("        dispatch after NV12 frames  = %llu (stagedCopies=%llu)\n",
                        (unsigned long long)r.dispatchAfterCompanion,
                        (unsigned long long)r.stagedAfterCompanion);
            std::printf("        STAGED-INPUT SUBMIT ENGINE  = %s (VK_QUEUE flags 0x%x) "
                        "queueFamilyIndex=%u\n",
                        SubmitEngineName(r.submitFlagsCompanion),
                        r.submitFlagsCompanion, r.submitFamilyCompanion);
            std::printf("        staging acquires: foreign=%llu local=%llu "
                        "(after primary: foreign=%llu local=%llu)\n",
                        (unsigned long long)r.foreignAcqCompanion,
                        (unsigned long long)r.localAcqCompanion,
                        (unsigned long long)r.foreignAcqPrimary,
                        (unsigned long long)r.localAcqPrimary);
            if (!r.companionError.empty()) {
                std::printf("        companionError: %s\n", r.companionError.c_str());
            }
            // THE TWO VERDICTS, each a failure and not a note.
            if (!r.companionRegistered) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- the session refused an NV12 descriptor, "
                            "so declaring RGBA COSTS the direct lane\n");
            } else if (r.companionPath != r.companionExpectPath) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- NV12 routed %s, not %s\n",
                            PathName(r.companionPath),
                            PathName(r.companionExpectPath));
            } else if (r.stagedCompanion &&
                       (r.stagedAfterCompanion - r.stagedAfterPrimary) != kFrames) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- %llu of %u NV12 frames took the staging "
                            "COPY arm; a staged companion that does not copy is measuring "
                            "nothing\n",
                            (unsigned long long)(r.stagedAfterCompanion -
                                                 r.stagedAfterPrimary),
                            kFrames);
            } else if (r.companionSubmitted != kFrames) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- only %u of %u NV12 frames submitted\n",
                            r.companionSubmitted, kFrames);
            } else if (r.dispatchAfterCompanion != r.dispatchAfterPrimary) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- the NV12 frames DISPATCHED the filter "
                            "(%llu -> %llu); routing is per-session, not per-frame\n",
                            (unsigned long long)r.dispatchAfterPrimary,
                            (unsigned long long)r.dispatchAfterCompanion);
            } else if (r.dispatchAfterPrimary == 0 && row.arm == ARM_FILTER_RGBA) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- the RGBA frames did NOT dispatch the "
                            "filter, so the companion comparison proves nothing\n");
            } else if (g_companionForeign &&
                       (r.foreignAcqCompanion - r.foreignAcqPrimary) != kFrames) {
                g_failures++;
                std::printf("        VERDICT: FAIL -- --companion-foreign asked for %u FOREIGN "
                            "acquires and got %llu; with no foreign acquire there is no "
                            "foreign RELEASE, so the queue-family claim is untested\n",
                            kFrames,
                            (unsigned long long)(r.foreignAcqCompanion -
                                                 r.foreignAcqPrimary));
            } else if (r.stagedCompanion && row.arm == ARM_DIRECT) {
                // ARM A. There is no filter on this session and there is not
                // meant to be, so "RGBA dispatched" is not a claim this row can
                // make. What it establishes is the CONTROL VALUE of the submit
                // engine, and it is a failure if it is not the encode or
                // transfer family -- that would mean the control and the
                // subject were never different and the A/B measures nothing.
                if (r.submitFlagsCompanion != VK_QUEUE_VIDEO_ENCODE_BIT_KHR &&
                    r.submitFlagsCompanion != VK_QUEUE_TRANSFER_BIT) {
                    g_failures++;
                    std::printf("        VERDICT: FAIL -- arm A (no filter) submitted staged "
                                "input on %s; expected VIDEO_ENCODE or TRANSFER\n",
                                SubmitEngineName(r.submitFlagsCompanion));
                } else {
                    std::printf("        VERDICT: PASS (arm A control) -- no filter, %u staged "
                                "copies, 0 dispatches, staged input on %s family %u\n",
                                kFrames, SubmitEngineName(r.submitFlagsCompanion),
                                r.submitFamilyCompanion);
                }
            } else {
                std::printf("        VERDICT: PASS -- RGBA dispatched %llu, NV12 dispatched 0, "
                            "one session, per-frame routing\n",
                            (unsigned long long)r.dispatchAfterPrimary);
            }
        }
        // THE VERDICT MUST READ EVERY SIGNAL THE ROW ALREADY PRODUCED.
        // "bytes > 0 && retrieved > 0" alone passed a row that lost the GPU
        // mid-encode, that dropped nine of twelve frames in the assembly
        // pipeline, that broke out of the submit loop on an error status, or
        // that wrote its bitstream nowhere. Each of those was measured and
        // printed one line above and then discarded.
        if (r.deviceLost) {
            g_failures++;
            std::printf("      RESULT: DEVICE LOST during encode -- bytes=%llu is not a pass\n\n",
                        (unsigned long long)r.bytes);
        } else if (r.fileOpenFailed) {
            g_failures++;
            std::printf("      RESULT: could not open the output file; nothing was written "
                        "for a decoder to judge\n\n");
        } else if (r.retrieved < r.submitted) {
            g_failures++;
            std::printf("      RESULT: LOST FRAMES -- submitted=%u retrieved=%u after the "
                        "bounded wait\n\n", r.submitted, r.retrieved);
        } else if (r.submitted < kFrames) {
            g_failures++;
            std::printf("      RESULT: SUBMIT ABORTED after %u of %u frames "
                        "(lastSubmitStatus=%d)\n\n",
                        r.submitted, kFrames, r.lastSubmitStatus);
        } else if (r.bytes > 0 && r.retrieved > 0) {
            nEncoded++;
            std::printf("      RESULT: ENCODED -> %s\n", r.file.c_str());
            // THE DECODE ASSERTION, IN THE DEFAULT CHAIN. Not behind a flag:
            // the state it exists to catch -- the compute filter recording its
            // dispatches and never executing them -- is green on every other
            // observable this suite has, so an opt-in gate would simply not be
            // opted into on the run that mattered.
            std::string qdetail;
            const QuadStatus qs = CheckQuadrants(r.file, &qdetail);
            if (qs == QUAD_PASS) {
                g_quadChecked++;
                std::printf("      DECODE GATE: PASS --%s\n\n", qdetail.c_str());
            } else if (qs == QUAD_NO_DECODER) {
                // NOT A PASS AND NOT SILENT. The run is downgraded to SKIPPED
                // at exit; see the bottom of main().
                g_quadNoDecoder = true;
                std::printf("      DECODE GATE: NOT RUN -- %s. This row's colour "
                            "is UNJUDGED.\n\n", qdetail.c_str());
            } else {
                g_quadChecked++;
                g_quadFailed++;
                g_failures++;
                std::printf("      DECODE GATE: FAIL -- %s\n\n", qdetail.c_str());
            }
        } else {
            g_failures++;
            std::printf("      RESULT: NO BITSTREAM (registered and routed, produced nothing)\n\n");
        }
    }

    std::printf("================================================================\n");
    // decodeGated IS PART OF THE BAR, not decoration. A run in which the
    // colour assertion did not execute on every encoded row is not the same
    // run as one in which it did, and without this number on the summary line
    // the two are indistinguishable from a CI log.
    // deviceLimited and av1Unverified are APPENDED, not inserted: every
    // existing CI line that greps the first six fields still matches. They
    // are on the bar for the same reason decodeGated is -- a run in which an
    // arm was refused by the device is not the same run as one in which it
    // encoded, and without the number on this line the two are
    // indistinguishable from a log.
    std::printf("sessions=%zu encoded=%zu abandoned=%zu failures=%d "
                "decodeGated=%d decodeFailed=%d deviceLimited=%zu "
                "av1Unverified=%zu\n",
                nSession, nEncoded, nAbandoned, g_failures,
                g_quadChecked, g_quadFailed, nDeviceLimited, nAv1Unverified);
    if (!g_av1ProbeDetail.empty()) {
        std::printf("av1 capability probe: %s\n", g_av1ProbeDetail.c_str());
    }
    if (g_ffmpegProbe == 1) {
        std::printf("decoder: %s\n", g_ffmpegBanner.c_str());
    }

    // THIS USED TO `return 0;` UNCONDITIONALLY, WHICH MADE THE ONLY TEST IN
    // THE SUITE THAT ENCODES REAL PIXELS INCAPABLE OF GOING RED.
    //
    // g_failures was counted at two sites -- an upload failure and a row that
    // registered, routed, and produced NO BITSTREAM -- printed on the line
    // above, and then discarded. The second of those is the exact failure this
    // file's own header says it exists to catch, the one the routing test
    // cannot see. It carries LABELS "gpu" and is therefore in the CI gating
    // set, so a green run here has meant nothing.
    //
    // THE SKIP MUST BE JUSTIFIED BY A DEVICE SIGNAL, NOT BY AN AGGREGATE.
    //
    // An earlier version skipped on nSession == 0 alone. That is not "the
    // GPU-less host": it is also "the library refused every session", and the
    // two are trivially confusable. Configure with
    // -DBUILD_ENCODER_COMPUTE_FILTER=OFF -- a supported option -- and
    // InitializeExt hard-refuses every row because this file sets
    // enablePreprocessFilter unconditionally; nSession is 0 on a box with a
    // working GPU, and a compute-shader regression, which is the single thing
    // this suite exists to catch, would have reported SKIPPED and left CI
    // green. Replacing "cannot fail" with "can be silenced" is not progress.
    //
    // VK_ERROR_INCOMPATIBLE_DRIVER is the loader's answer when no usable
    // driver is present -- it is what this binary returns for every row on a
    // GPU-less host, measured. Any OTHER failure came from the library with a
    // driver in hand and is a verdict, not an environment fact.
    if (nSession == 0) {
        bool everyRowSaysNoDriver = !rowResults.empty();
        // THE SECOND ENVIRONMENT CASE, which `--codec av1` on hardware
        // without AV1 produces:
        // every selected row was refused BY THE DEVICE. That is a skip for
        // the same reason an absent driver is, and treating it as a failure
        // would make the AV1 ctest entry red on every pre-Blackwell box.
        //
        // IT DOES NOT WEAKEN THE GUARD THE ORIGINAL COMMENT EXISTS FOR.
        // Configuring -DBUILD_ENCODER_COMPUTE_FILTER=OFF still turns this
        // suite RED, not skipped: that refusal comes out of the config binder
        // as VK_ERROR_INITIALIZATION_FAILED, which IsDeviceLimitedInit()
        // deliberately does not admit. And the skip requires g_failures == 0
        // and nAv1Unverified == 0, so it cannot launder a verdict or an
        // unjudged row -- the same invariant the ffmpeg skip below keeps.
        bool everyRowRefusedByDevice =
            !rowResults.empty() && (g_failures == 0) && (nAv1Unverified == 0);
        for (const VkResult rr : rowResults) {
            if (rr != VK_ERROR_INCOMPATIBLE_DRIVER) {
                everyRowSaysNoDriver = false;
                if (!IsDeviceLimitedInit(rr)) {
                    everyRowRefusedByDevice = false;
                }
            }
        }
        if (everyRowSaysNoDriver) {
            std::printf("SKIP: no usable Vulkan driver -- every row returned "
                        "VK_ERROR_INCOMPATIBLE_DRIVER\n");
            return 77;
        }
        if (everyRowRefusedByDevice) {
            std::printf("SKIP: every selected row was refused by the DEVICE, "
                        "not by the library -- %zu of %zu selected row(s) "
                        "device-limited. There is a working driver here; it "
                        "does not implement what these rows ask for.\n",
                        nDeviceLimited, nSelected);
            if (!g_av1ProbeDetail.empty()) {
                std::printf("      %s\n", g_av1ProbeDetail.c_str());
            }
            return 77;
        }
        std::printf("FAILED: no session was created, and at least one row "
                    "failed for a reason that is neither a missing driver nor "
                    "a device limitation -- that is a library refusal, not an "
                    "absent GPU\n");
        for (size_t i = 0; i < rowResults.size(); i++) {
            std::printf("        row %zu InitializeExt -> %d\n", i, (int)rowResults[i]);
        }
        return 1;
    }

    if (g_failures != 0) {
        return 1;
    }

    // A GATE THAT PASSES WHEN ITS DECODER IS MISSING IS ANOTHER CANNOT-FAIL
    // TEST. Every counter in this suite is green with the compute filter dead,
    // so "we encoded seven files and looked at none of them" must not be
    // reported as a pass. It is reported as SKIPPED -- 77, which CMakeLists.txt
    // already declares as this test's SKIP_RETURN_CODE -- with the reason on
    // stdout. Real failures above still return 1 first, so this cannot be used
    // to launder one.
    if (g_quadNoDecoder) {
        std::printf("SKIP: ffmpeg is not on PATH, so the four-quadrant decode "
                    "assertion did not run on %zu encoded row(s). That assertion "
                    "is the ONLY observable in this suite that can see the "
                    "compute filter stop executing -- with the filter's four "
                    "vkCmdDispatch calls deleted, every counter above still "
                    "reads green. An unjudged run is not a pass.\n", nEncoded);
        return 77;
    }

    // Belt and braces: rows encoded but nothing graded means the wiring above
    // was bypassed, which is the failure this whole change exists to remove.
    if (nEncoded != 0 && g_quadChecked == 0) {
        std::printf("FAILED: %zu row(s) encoded and the four-quadrant decode "
                    "assertion ran on none of them\n", nEncoded);
        return 1;
    }
    return 0;
}
