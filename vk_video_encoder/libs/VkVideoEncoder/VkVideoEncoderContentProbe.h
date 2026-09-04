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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERCONTENTPROBE_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERCONTENTPROBE_H_

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanVideoImagePool.h"
#include "vulkan/vulkan.h"

class VulkanDeviceContext;

// The dma-buf IMPORT CONTENT PROBE.
//
// WHAT IT IS FOR, in one sentence: a dma-buf import can come back bound to
// memory the producer's writes never reach, and this is the only
// place in the library that can SEE that, because it is the only place that
// reads imported pixels on the host.
//
// The contract, the exact predicate, the false-positive budget and the
// latency cost are all stated at VkVideoEncoderImportContentInfo, in the
// descriptor API's own header. Named by symbol rather than by file: this
// class sits below that layer and must not depend on where it is declared.
// This header does not restate the contract; it describes the mechanism.
//
// THE MECHANISM, and why it is shaped this way.
//
//   * The capture is TWO vkCmdCopyImage into the caller's OWN staging command
//     buffer, one command after VkVideoEncoder::StageInputFrame's
//     CopyLinearToOptimalImage has read the same image in the same layout.
//     No extra submit, no extra fence, no extra queue. The imported image is
//     already in TRANSFER_SRC_OPTIMAL at that point -- the staging arm put it
//     there -- so only the destination needs a barrier.
//
//   * The scoring is host-side, off a HOST_VISIBLE|HOST_COHERENT LINEAR pool
//     image, and MUST run only after that command buffer's fence has been
//     waited. VkVideoEncoder calls it from the same two post-fence sites the
//     PSNR readback uses.
//
//   * ROW STRIDE. Only every kRowStride-th row is touched, and that is a
//     requirement rather than a micro-optimisation: a byte-wise walk of this
//     same 3.1 MB of HOST_VISIBLE memory costs VkVideoEncoderPsnr the
//     difference between ~200 fps and 3.6 fps. A strided mean is ample for
//     "is this plane dead", which is the only question asked.
//
//   * ONCE PER REGISTRATION. Not once per frame. The defect is a property of
//     the IMPORT, is permanent for the life of that buffer (a damaged import
//     is not recoverable, and re-importing at the next ordinal rescues it in
//     0 of 8 attempts), and 100% of a damaged buffer's frames carry it -- so a
//     second look costs a readback and can learn nothing. On a 5125-frame
//     session with 5 registered buffers this is 5 probes, not 5125.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not write to stderr. A host that
// silences stdio, or that runs the encoder in a child process whose stderr it
// never reads, would lose every finding -- and a diagnostic whose only signal
// can be discarded by the caller it exists for tells that caller nothing.
// Every answer leaves through the chained struct, which the caller must read.
class VkVideoEncoderContentProbe : public VkVideoRefCountBase {
public:
    // Mirrors VkVideoEncoderImportContentState in the public header. Kept as
    // a separate enum because this class sits BELOW the ext layer and must not
    // depend on it; vulkan_video_encoder_ext.cpp maps one onto the other, and
    // a static_assert there pins the mapping.
    enum State {
        STATE_NOT_EVALUATED  = 0,
        STATE_NOT_APPLICABLE = 1,
        STATE_ARMED          = 2,
        STATE_CLEAN          = 3,
        STATE_DAMAGED_CHROMA = 4,
        STATE_DAMAGED_ALL    = 5,
    };

    // Whether the probe's readback can ride a given registration at all.
    //
    // A SCOPED ENUM AND NOT A BOOL, on purpose. Its true-ish value means "DO
    // arm", which is the opposite sense of the `bool directlyEncodable` a caller
    // reaching for the obvious spelling would pass, whose true means "do not
    // arm". A bool lets a call site compile with its meaning inverted -- arming
    // what should be refused and refusing what should be armed -- and says
    // nothing. An enum class makes each of those a compile error.
    enum class CaptureSite {
        kUnreachable = 0,
        kReachable   = 1,
    };

    // The predicate's threshold in Q8 (plane mean * 256): strictly below
    // 2.0/255. Must equal VK_VIDEO_ENCODER_IMPORT_CONTENT_DEAD_PLANE_MEAN_Q8;
    // vulkan_video_encoder_ext.cpp static_asserts that.
    static constexpr uint32_t kDeadPlaneMeanQ8 = 512u;

    // Travels on VkVideoEncodeFrameInfo from the record site (submit thread)
    // to the score site (assembly thread, post-fence).
    struct FrameCapture {
        VkSharedBaseObj<VulkanVideoImagePoolNode> image;
        uint64_t registrationId = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
    };

    struct Verdict {
        State    state = STATE_NOT_EVALUATED;
        uint64_t registrationId = 0;
        uint32_t meanY = 0;  // Q8
        uint32_t meanU = 0;  // Q8
        uint32_t meanV = 0;  // Q8
    };

    static VkResult Create(VkSharedBaseObj<VkVideoEncoderContentProbe>& probe);

    // Idempotent; called from VkVideoEncoder::InitEncoder. Allocates nothing:
    // the image pool is configured lazily on the first capture, so a session
    // that arms no registration pays no memory at all.
    void Configure(const VulkanDeviceContext* vkDevCtx,
                   uint32_t poolDepth,
                   uint32_t queueFamilyIndex,
                   uint32_t encodeWidth,
                   uint32_t encodeHeight);

    // ---- Arming, from the registration thread -------------------------
    //
    // |captureSiteReachable| is the ONE fact that decides whether a
    // registration can be probed, and it is deliberately NOT "is this buffer
    // directly encodable". Coupling the two would make the probe
    // structurally blind to the class of buffer the driver defect appears
    // on: a BLOCK-LINEAR import that also carries VIDEO_ENCODE_SRC
    // classifies encodeCapable -- because encodeCapable's middle clause is
    // (tiling != VK_IMAGE_TILING_LINEAR) -- and encodeCapable would latch
    // NOT_APPLICABLE here, while block-linear imports are exactly the class
    // that gets poisoned. Tiling plays no part in this decision.
    //
    // What replaces it is the only thing that actually decides it: does a
    // transfer-readable copy of the PRODUCER'S pixels pass through this
    // library for this registration. The DIRECT path now makes that true for
    // itself with a one-frame staged detour, taken only while a capture is
    // still owed (VkVideoEncoder::SetExternalInputFrameWithNode).
    //
    // A registration whose capture site is genuinely unreachable -- a
    // FILTER-routed one, which is sampled and never copied, or an import
    // without TRANSFER_SRC, out of which no copy may legally be recorded --
    // is armed as NOT_APPLICABLE rather than refused: the caller asked, and
    // "your buffer never takes the path this probe is hooked to" is an
    // answer, not an error.
    void ArmRegistration(uint64_t registrationId, CaptureSite captureSite);

    // Whether the SCORER can read this format at all -- a pure function of
    // the format, holding no state and needing no lock.
    //
    // PUBLIC BECAUSE THE ARM DECISION HAS TO ASK IT. Consulting it only from
    // RecordCapture, on the first frame, lets a registration in a format the
    // scorer cannot read echo ARMED at import and then be downgraded to
    // NOT_APPLICABLE a frame later, which leaves a session indistinguishable from
    // one that probed every buffer and found them all clean. An ARM is a promise
    // of a verdict; a promise the scorer cannot keep must be refused where it is
    // made.
    static bool IsProbeableFormat(VkFormat format);
    // Drop everything remembered about a registration. Called from
    // UnregisterImageResource, which is also what makes the damaged-list
    // report in GetSnapshot() drain as a caller reacts to it.
    void ForgetRegistration(uint64_t registrationId);
    State GetRegistrationState(uint64_t registrationId) const;
    bool IsArmed() const;

    // ---- Capture, from the submit thread ------------------------------
    //
    // True only while this registration is armed and has neither been
    // captured nor scored. Cheap enough to call per frame.
    bool NeedsCapture(uint64_t registrationId) const;
    // Records the readback into |cmdBuf| and hands back the pool node in
    // |outCapture|. Returns false without touching |cmdBuf| when the probe
    // cannot run (unsupported format, pool exhausted, not configured), and
    // latches NOT_APPLICABLE for an unsupported format so the caller is not
    // asked again every frame.
    bool RecordCapture(VkCommandBuffer cmdBuf,
                       uint64_t registrationId,
                       VkImage srcImage,
                       VkFormat srcFormat,
                       const VkExtent2D& srcExtent,
                       FrameCapture& outCapture);

    // ---- Scoring, from the assembly thread, POST-FENCE ----------------
    //
    // Consumes |capture| (releases the pool node) whether or not it scores.
    void ScoreCapture(FrameCapture& capture);

    // The two halves of the scoring, split out as PURE FUNCTIONS -- and the
    // split exists so they can be tested at all.
    //
    // Everything else about a probe needs a Vulkan device: a real import, a
    // real staging command buffer, a real fence. The DECISION does not, and
    // it is the part that has to be right. A driver-workaround predicate that
    // is only exercised on the one host that has the broken driver is a
    // predicate nobody can regression-test, and this project has already
    // shipped observables whose test could not fail. These two take bytes and
    // numbers, and test/encoder-ext-import-content drives them over
    // synthetic NV12 buffers -- chroma-zeroed, all-zero, legal black and
    // ordinary content -- on any host, with no GPU.

    // Reads a HOST-VISIBLE, LINEAR, 8-bit 2-plane 420 image and returns the
    // three plane means in Q8 (mean * 256). |base| is the mapped memory,
    // |lumaLayout| and |chromaLayout| the VkSubresourceLayout of PLANE_0 and
    // PLANE_1 -- offsets and row pitches included, because a real image is
    // padded and a scorer that assumes width == rowPitch reads the padding.
    // |scratch| is caller-owned reusable storage; see kRowStride.
    static void ScorePlanesQ8(const uint8_t* base,
                              const VkSubresourceLayout& lumaLayout,
                              const VkSubresourceLayout& chromaLayout,
                              uint32_t width, uint32_t height,
                              std::vector<uint8_t>& scratch,
                              uint32_t& outMeanYQ8,
                              uint32_t& outMeanUQ8,
                              uint32_t& outMeanVQ8);

    // THE PREDICATE. The union of the two measured damage modes; see the
    // definition for the full argument, including the quadrant it does not
    // cover and why.
    static State ClassifyPlaneMeansQ8(uint32_t meanYQ8, uint32_t meanUQ8,
                                      uint32_t meanVQ8);

    // Latch a measurement against a registration: classify it, record the
    // verdict, and move the session counters. Public because ScoreCapture is
    // not the only legitimate caller -- it is the second half of scoring, and
    // separating "get the numbers" from "act on the numbers" is what lets the
    // latch, the damaged ordering and the drain-on-retirement be exercised
    // without a device. A no-op for a registration that is not ARMED.
    void ApplyVerdict(uint64_t registrationId, uint32_t meanYQ8,
                      uint32_t meanUQ8, uint32_t meanVQ8);

    // ---- Reporting, from any thread -----------------------------------
    //
    // Reports the OLDEST STILL-REGISTERED damaged verdict when there is one,
    // and otherwise the most recent CLEAN verdict. See the public header for
    // why that ordering, and not "most recent", is the one a caller can act
    // on without losing a verdict between two polls.
    //
    // |outArmedCount| is THE ANSWER TO "DID ANY OF THIS ACTUALLY RUN", and it
    // exists because without it this probe had the same disguised-inertness
    // shape it was built to expose. A registration that arms and is never
    // captured stays STATE_ARMED forever; GetSnapshot then reports the
    // NOT_EVALUATED default verdict with probed=0 damaged=0 -- which is
    // BYTE-IDENTICAL to a session that armed nothing at all, and only one
    // step away from "armed everything and every buffer was clean". A probe
    // whose failure mode reads as success is not a detector.
    //
    // It is a COUNT OF LIVE REGISTRATIONS IN STATE_ARMED, not a session
    // total, and that is the useful direction: it falls to zero as captures
    // land, so "still non-zero at the end of a session" is the exact
    // statement "these buffers were promised a verdict and never got one".
    // The one-frame staged detour in VkVideoEncoder::SetExternalInputFrame-
    // WithNode is what drives it to zero for a DIRECT registration, so this
    // is also the number that goes wrong first if that detour ever stops
    // firing -- which is the regression C1b in
    // test/encoder-ext-import-content could not otherwise catch.
    void GetSnapshot(Verdict& outVerdict,
                     uint32_t& outProbedCount,
                     uint32_t& outDamagedCount,
                     uint32_t& outArmedCount) const;

    void Deinit();

    ~VkVideoEncoderContentProbe() override;

private:
    // Every kRowStride-th row of each plane is read. See the class comment.
    static constexpr uint32_t kRowStride = 8u;

    struct Registration {
        State state = STATE_ARMED;
        // Set while a capture for this registration is recorded but not yet
        // scored, so a second frame of the same registration does not queue a
        // second readback behind the first.
        bool  capturePending = false;
    };

    // Callers hold m_mutex.
    static bool IsProbeableFormatLocked(VkFormat format);
    void ApplyVerdictLocked(uint64_t registrationId, uint32_t meanYQ8,
                            uint32_t meanUQ8, uint32_t meanVQ8);

    const VulkanDeviceContext* m_vkDevCtx = nullptr;
    uint32_t   m_poolDepth = 0;
    uint32_t   m_queueFamilyIndex = 0;
    VkExtent2D m_encodeExtent = {};

    mutable std::mutex m_mutex;
    VkSharedBaseObj<VulkanVideoImagePool> m_pool;
    VkFormat   m_poolFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_poolExtent = {};

    std::unordered_map<uint64_t, Registration> m_registrations;
    // Damaged registration ids in SCORING order. ForgetRegistration erases
    // from it; GetSnapshot reports its front.
    std::vector<uint64_t> m_damagedOrder;
    // Verdicts, kept so a report can carry the arithmetic behind the state.
    std::unordered_map<uint64_t, Verdict> m_verdicts;
    Verdict  m_lastCleanVerdict;
    uint32_t m_probedCount = 0;
    uint32_t m_damagedCount = 0;
    // Reused across scorings; guarded by m_mutex like everything else. One
    // row of the widest plane, so the per-row memcpy has a fixed destination
    // and the mapped HOST_VISIBLE memory is read exactly once, sequentially.
    std::vector<uint8_t> m_scratch;
};

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERCONTENTPROBE_H_ */
