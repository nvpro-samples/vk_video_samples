/*
 * Copyright 2022 NVIDIA Corporation.
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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODER_H_
#define _VKVIDEOENCODER_VKVIDEOENCODER_H_

#include <assert.h>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkVideoEncoderDef.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/VulkanVideoSession.h"
#include "VkCodecUtils/VulkanVideoSessionParameters.h"
#include "VkCodecUtils/VulkanVideoImagePool.h"
#include "VkCodecUtils/VulkanBufferPool.h"
#include "VkCodecUtils/VulkanCommandBufferPool.h"
#include "VkCodecUtils/VulkanVideoReferenceCountedPool.h"
#include "VkVideoEncoder/VkVideoEncoderContentProbe.h"
#include "VkCodecUtils/VkBufferResource.h"
#include "VkCodecUtils/VulkanBistreamBufferImpl.h"
#include "VkCodecUtils/VkThreadSafeQueue.h"
#include "VkEncoderDpbH264.h"
#include "VkEncoderDpbAV1.h"
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
#include "VkCodecUtils/VulkanVideoEncodeDisplayQueue.h"
#include "VkShell/Shell.h"
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
#include "mio/mio.hpp"
#ifdef NV_AQ_GPU_LIB_SUPPORTED
#include "EncodeAqAnalyzes.h"
#endif // NV_AQ_GPU_LIB_SUPPORTED
#include "VkVideoEncoder/VkVideoEncoderPsnr.h"
#include "VkCodecUtils/VkVideoCrc.h"

class VkVideoEncoderH264;
class VkVideoEncoderH265;
class VkVideoEncoderAV1;

class VkVideoEncoder : public VkVideoRefCountBase {

public:

    // ------------------------------------------------------------------
    // FILTER-DISPATCH OBSERVABLE
    //
    // What ACTUALLY ran, readable from outside the library. Everything the
    // ext layer can otherwise see is the CONFIG
    // (enablePreprocessFilter): a request, not an outcome. The two diverge
    // in both directions -- a session can configure the filter and then
    // route every frame down the staging copy (StageInputFrame's
    // `useComputeFilter` is a PER-FRAME predicate, not a session property),
    // and the copy arm is exactly the arm that hangs the GPU on a 3-plane
    // source -- so "did the filter run" cannot be inferred from the flag.
    //
    // Counted at the RECORD SITE, immediately after
    // VulkanFilter::RecordCommandBuffer() returns VK_SUCCESS, which is the
    // same site the hardware proof of this filter counted at. A filter that
    // failed to record does not count.
    //
    // BOTH ARMS ARE COUNTED SEPARATELY, on purpose. A single total plus a
    // subtraction is how a live tier already read as dead once on this
    // project (staging_frames_submitted_ is a superset of the cpu-dmabuf
    // count). These two never overlap: they are the two sides of one `if`.
    //
    // The counters are cumulative for the life of the encoder object and
    // are NOT reset by DeinitEncoder(), matching the diagnostic channel's
    // documented "never resets" rule -- a teardown-time read is the one
    // most likely to matter.
    enum InputFilterKind {
        INPUT_FILTER_NONE          = 0,
        INPUT_FILTER_YCBCR_COPY    = 1,  // VulkanFilterYuvCompute::YCBCRCOPY
        INPUT_FILTER_RGBA_TO_YCBCR = 2,  // VulkanFilterYuvCompute::RGBA2YCBCR
        INPUT_FILTER_YCBCR_TO_RGBA = 3,  // VulkanFilterYuvCompute::YCBCR2RGBA
    };

    // The filter OBJECT exists on this session. Deliberately distinct from
    // the config flag: InitEncoder hard-fails when those two can diverge,
    // and this is what proves that hard-fail is holding.
    bool HasInputComputeFilter() const {
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
        return (m_inputComputeFilter != nullptr);
#else
        return false;
#endif
    }
    uint32_t GetInputFilterKind() const {
        return m_inputFilterKind.load(std::memory_order_relaxed);
    }
    uint64_t GetInputFilterDispatchCount() const {
        return m_inputFilterDispatchCount.load(std::memory_order_relaxed);
    }
    uint64_t GetStagedCopyCount() const {
        return m_stagedCopyCount.load(std::memory_order_relaxed);
    }
    // The dma-buf import content probe, INJECTED by the ext layer -- see the
    // note on m_contentProbe for why it is not owned here. Null on a session
    // whose caller never opted in, and inert (allocating no device memory)
    // until a capture is actually recorded.
    const VkSharedBaseObj<VkVideoEncoderContentProbe>& GetContentProbe() const {
        return m_contentProbe;
    }
    // Idempotent. Safe to call before or after InitEncoder: whichever runs
    // second performs the Configure, so the ext layer does not have to know
    // which order CreateVideoEncoder left things in.
    void SetContentProbe(const VkSharedBaseObj<VkVideoEncoderContentProbe>& probe) {
        m_contentProbe = probe;
        ConfigureContentProbe();
    }
    // The two sides of the staging ACQUIRE decision, reported through
    // VkVideoEncoderInputResidencyInfo. See the header for why a validation
    // count cannot substitute for these: both barrier programs are
    // spec-clean and leave the image in the same layout.
    uint64_t GetForeignAcquireCount() const {
        return m_foreignAcquireCount.load(std::memory_order_relaxed);
    }
    uint64_t GetLocalAcquireCount() const {
        return m_localAcquireCount.load(std::memory_order_relaxed);
    }

    // THE STAGED-INPUT SUBMIT FAMILY, reported through
    // VkVideoEncoderInputResidencyInfo. Public wrappers over the two
    // protected accessors declared further down (GetStagedInputSubmitType /
    // GetStagedInputQueueFamilyIdx) so an out-of-library caller can read the
    // fact those two exist to keep in agreement, WITHOUT re-deriving it.
    //
    // WHY THIS IS OBSERVABLE AT ALL, stated here because "it encoded" is not
    // the question these answer. GetStagedInputSubmitType() returns COMPUTE
    // whenever a preprocess filter OBJECT exists on the session -- a session
    // property, not a per-frame one -- so a frame that dispatches NO filter
    // and takes the plain-copy arm still has its acquire, its
    // ReleaseImageToForeignQueue and its submit moved onto the compute family
    // the moment the session gains a filter. Whether that move happened is
    // invisible in a bitstream, invisible in a byte count, and invisible to
    // the validation layer. It is exactly the fact that has to be read to
    // know whether widening a session's declared input format moved the NV12
    // lane off the encode engine, which matters because a FOREIGN release
    // recorded off the wrong engine can lose the device (see
    // ReleaseImageToForeignQueue).
    //
    // Returned as the raw VK_QUEUE_* flag bit rather than the internal enum
    // so no ext-layer translation table can drift from it.
    uint32_t GetStagedInputSubmitTypeFlag() const {
        return (uint32_t)GetStagedInputSubmitType();
    }
    uint32_t GetStagedInputSubmitQueueFamilyIdx() const {
        return GetStagedInputQueueFamilyIdx();
    }

    using VulkanBitstreamBufferPool = VulkanVideoRefCountedPool<VulkanBitstreamBufferImpl, 64>;

    enum { MAX_IMAGE_REF_RESOURCES = 17 }; /* List of reference pictures 16 + 1 for current */
    enum { MAX_BITSTREAM_HEADER_BUFFER_SIZE = 256 };

    // Queue-family ownership of an external input image. Internal
    // mirror of the public VkVideoEncoderInputResidency (the Ext API maps
    // one onto the other); AUTO keeps the legacy layout-based inference.
    enum ExternalInputResidency {
        EXTERNAL_INPUT_RESIDENCY_AUTO    = 0,
        EXTERNAL_INPUT_RESIDENCY_LOCAL   = 1,
        EXTERNAL_INPUT_RESIDENCY_FOREIGN = 2,
    };

    struct BitstreamReadback {
        uint32_t bitstreamStartOffset{0};
        uint32_t bitstreamSize{0};
        VkQueryResultStatusKHR status{VK_QUERY_RESULT_STATUS_NOT_READY_KHR};
        std::vector<uint8_t> bitstreamCopy;
        bool readbackDone{false};
    };

    // ============================================================================
    // Timeline Semaphore Synchronization Helpers
    // ============================================================================

    /**
     * @brief Synchronization state indices for timeline semaphores and fence sets
     *
     * These enum values serve are used for timeline semaphore values
     * for GPU-side synchronization
     *
     * Each processing stage signals its completion by
     * incrementing timeline semaphore
     */
    enum SyncState {
        SYNC_INPUT_PREPROCESSING_COMPLETE = 1, // Copy linear buffer to optimal, downsample, etc.
        SYNC_AQ_PROCESSING_COMPLETE,           // AQ processing
        SYNC_ENCODE_PROCESSING_COMPLETE,       // QP delta generation (temporal)
        SYNC_ASSEMBLY_PROCESSING_COMPLETE,     // bitstream post processing and assembly
        SYNC_PROCESSING_STATE_COUNT            // Total number of sync states
    };

    /**
     * @brief Shift amount for encoding stage in timeline semaphore value
     *
     * We have 4 sync stages (SYNC_PROCESSING_STATE_COUNT = 5), requiring 3 bits (2^3 = 8).
     * Timeline value = (frameNumber << SEM_SYNC_TYPE_IDX_SHIFT) | stage
     *
     */
    static constexpr uint64_t SEM_SYNC_TYPE_IDX_SHIFT = 3;

    /**
     * @brief Calculate timeline semaphore value from frame number and stage
     * @param stage Processing stage preprocess, AQ, encode, assembly
     * @param frameNumber Frame sequence number (inputSeqNumber)
     * @return Encoded timeline semaphore value
     */
    static inline uint64_t GetSemaphoreValue(SyncState stage, uint64_t frameNumber) {
        return (frameNumber << SEM_SYNC_TYPE_IDX_SHIFT) | static_cast<uint64_t>(stage);
    }

    /**
     * @brief Extract frame number from timeline semaphore value
     * @param semaphoreValue Encoded timeline value
     * @return Frame number
     */
    static inline uint64_t GetFrameNumberFromSemaphore(uint64_t semaphoreValue) {
        return semaphoreValue >> SEM_SYNC_TYPE_IDX_SHIFT;
    }

    /**
     * @brief Extract stage from timeline semaphore value
     * @param semaphoreValue Encoded timeline value
     * @return Processing stage
     */
    static inline SyncState GetStageFromSemaphore(uint64_t semaphoreValue) {
        uint64_t mask = (1ULL << SEM_SYNC_TYPE_IDX_SHIFT) - 1;
        return static_cast<SyncState>(semaphoreValue & mask);
    }

    // Static assert: Ensure SYNC_PROCESSING_STATE_COUNT fits within the shift amount
    static_assert(SYNC_PROCESSING_STATE_COUNT <= (1ULL << SEM_SYNC_TYPE_IDX_SHIFT),
                  "AQ_SYNC_STATE_COUNT must be <= 1 << SEM_SYNC_TYPE_IDX_SHIFT");

    struct VkVideoEncodeFrameInfo : public VkVideoRefCountBase
    {
        inline VkVideoCodecOperationFlagBitsKHR GetType() const {
            return m_codec;
        }

        VkVideoEncodeFrameInfo(const void* pNext = nullptr,
                               VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_NONE_KHR)
            : encodeInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR, pNext}
            , quantizationMapInfo()
            , intraRefreshInfo()
            , frameInputOrderNum(uint64_t(-1))
            , frameEncodeInputOrderNum(uint64_t(-1))
            , frameEncodeEncodeOrderNum(uint64_t(-1))
            , gopPosition(uint32_t(-1))
            , picOrderCntVal(-1)
            , inputTimeStamp(0)
            , bitstreamHeaderBufferSize(0)
            , bitstreamHeaderOffset(0)
            , bitstreamHeaderBuffer{}
            , constQp()
            , qualityLevel()
            , islongTermReference(false)
            , sendControlCmd(false)
            , sendResetControlCmd(false)
            , sendQualityLevelCmd(false)
            , sendRateControlCmd(false)
            , lastFrame(false)
            , numDpbImageResources()
            , controlCmd()
            , pControlCmdChain(nullptr)
            , qualityLevelInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR }
            , rateControlInfo { VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR }
            , rateControlLayersInfo{{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR }}
            , referenceSlotsInfo{}
            , referenceIntraRefreshInfo{}
            , setupReferenceSlotInfo{ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR }
            , videoSession()
            , videoSessionParameters()
            , srcStagingImageView()
            , srcEncodeImageResource()
            , setupImageResource()
#if NV_AQ_GPU_LIB_SUPPORTED
            , subsampledImageResource()
#endif // NV_AQ_GPU_LIB_SUPPORTED
            , outputBitstreamBuffer()
            , dpbImageResources()
            , srcQpMapStagingResource()
            , srcQpMapImageResource()
            , qpMapCmdBuffer()
            , m_parent(nullptr)
            , m_parentIndex(-1)
            , m_codec(codec)
        {
            assert(ARRAYSIZE(referenceSlotsInfo) == MAX_IMAGE_REF_RESOURCES);
            for (uint32_t i = 0; i < MAX_IMAGE_REF_RESOURCES; i++) {
                referenceSlotsInfo[i].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            }
            for (uint32_t i = 0; i < MAX_IMAGE_REF_RESOURCES; i++) {
                referenceIntraRefreshInfo[i].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_INTRA_REFRESH_INFO_KHR;
                referenceIntraRefreshInfo[i].pNext = nullptr;
            }
            assert(numDpbImageResources <= ARRAYSIZE(dpbImageResources));
            for (uint32_t i = 0; i < numDpbImageResources; i++) {
               dpbImageResources[i] = nullptr;
            }
            numDpbImageResources = 0;
        }

        VkVideoEncodeInfoKHR                               encodeInfo;
        VkVideoEncodeQuantizationMapInfoKHR                quantizationMapInfo;
        VkVideoEncodeIntraRefreshInfoKHR                   intraRefreshInfo;
        uint64_t                                           frameInputOrderNum;          // == encoder input order in sequence
        uint64_t                                           frameEncodeInputOrderNum;    // == encoder input order sequence number
        uint64_t                                           frameEncodeEncodeOrderNum;   // == encoder encode order sequence number
        VkVideoGopStructure::GopPosition                   gopPosition;
        int32_t                                            picOrderCntVal;
        uint64_t                                           inputTimeStamp;
        size_t                                             bitstreamHeaderBufferSize;
        uint32_t                                           bitstreamHeaderOffset;
        uint8_t                                            bitstreamHeaderBuffer[MAX_BITSTREAM_HEADER_BUFFER_SIZE];
        ConstQpSettings                                    constQp;
        uint32_t                                           qualityLevel;
        uint32_t                                           islongTermReference : 1;
        uint32_t                                           sendControlCmd      : 1;
        uint32_t                                           sendResetControlCmd : 1;
        uint32_t                                           sendQualityLevelCmd : 1;
        uint32_t                                           sendRateControlCmd  : 1;
        uint32_t                                           lastFrame           : 1;
        uint32_t                                           numDpbImageResources;
        VkVideoCodingControlFlagsKHR                       controlCmd;
        VkBaseInStructure *                                pControlCmdChain;
        VkVideoEncodeQualityLevelInfoKHR                   qualityLevelInfo;
        VkVideoEncodeRateControlInfoKHR                    rateControlInfo;
        VkVideoEncodeRateControlLayerInfoKHR               rateControlLayersInfo[1];
        VkVideoReferenceSlotInfoKHR                        referenceSlotsInfo[MAX_IMAGE_REF_RESOURCES];
        VkVideoReferenceIntraRefreshInfoKHR                referenceIntraRefreshInfo[MAX_IMAGE_REF_RESOURCES];
        VkVideoReferenceSlotInfoKHR                        setupReferenceSlotInfo;
        VkSharedBaseObj<VulkanVideoSession>                videoSession;
        VkSharedBaseObj<VulkanVideoSessionParameters>      videoSessionParameters;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcStagingImageView;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcEncodeImageResource;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          setupImageResource;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          subsampledImageResource; // 2x2 subsampled Y for AQ
        VkSharedBaseObj<VulkanBitstreamBuffer>             outputBitstreamBuffer;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          dpbImageResources[MAX_IMAGE_REF_RESOURCES];
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> inputCmdBuffer;
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> encodeCmdBuffer;
        VkSharedBaseObj<VkVideoEncodeFrameInfo>            dependantFrames;

        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcQpMapStagingResource;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcQpMapImageResource;
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> qpMapCmdBuffer;
        /** Per-frame PSNR capture/recon data (only used when PSNR is enabled). */
        VkVideoEncoderPsnr::FrameData                      psnrFrameData;
        /** The dma-buf import content probe's readback for THIS frame, held
         *  from the record site in StageInputFrame to the post-fence score.
         *  Empty on all but the first frame of an armed registration. */
        VkVideoEncoderContentProbe::FrameCapture           contentProbeCapture;
        /** The registration this frame was submitted against, 0 for the
         *  unregistered (legacy direct-image) arm. Carried because the
         *  content probe's unit is the REGISTRATION, not the frame: it is
         *  what latches "this buffer has already been looked at". */
        uint64_t                                           externalRegistrationId = 0;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
        std::shared_ptr<AqProcessor>                       aqProcessorSlot;
#endif // NV_AQ_GPU_LIB_SUPPORTED

        // === External frame input support ===
        // When isExternalInput is true, the srcStagingImageView was provided
        // externally (e.g. from DMA-BUF import) and is wrapped in a non-owning
        // VulkanVideoImagePoolNode via CreateExternal().
        // srcExternalImageLayout: actual layout the producer left the image in
        // (e.g. GENERAL for compute output). Must NOT be UNDEFINED or the
        // transition will discard image contents and produce scrambled encode.

        bool           isExternalInput{false};
        VkImageLayout  srcExternalImageLayout{VK_IMAGE_LAYOUT_UNDEFINED};

        // Did the CALLER state srcExternalImageLayout for THIS FRAME, or is
        // it the registration-time default standing in?
        //
        // The public contract makes the distinction, and the encoder core
        // cannot see it unaided: VkVideoEncoderFrameSubmitInfo::
        // currentLayout is documented as "the layout the producer left the
        // image in. VK_IMAGE_LAYOUT_UNDEFINED means 'as declared at
        // registration'", and the ext layer collapses both cases into one
        // value before calling down here. An EXPLICIT per-frame declaration
        // is the caller saying "I moved it", and it must beat the library's
        // own record of where it last left the image
        // (VulkanVideoImagePoolNode::GetStagedInputResidualLayout).
        //
        // false on the LEGACY SubmitExternalFrame lane and on the library's
        // own file-input lane, neither of which has a registration default
        // to stand in for -- and neither of which reads the residual either,
        // so the value is inert there.
        bool           srcExternalLayoutIsExplicit{false};

        // True when srcEncodeImageResource IS the caller's imported image
        // (Path A, zero-copy) rather than a library pool image that a staging
        // copy filled (Path B/C). Only Path A needs a queue-family acquire at
        // encode time: on B/C the encode reads library-owned memory that the
        // staging copy already acquired.
        bool           srcEncodeImageIsExternal{false};

        // THE LAYOUT StageInputFrame LEFT THE ENCODE-SOURCE IMAGE IN, as a
        // fact about a barrier this library recorded -- read once, by
        // RecordVideoCodingCmd, immediately before it opens the video coding
        // scope that will read that image.
        //
        // WHY THIS FIELD EXISTS AT ALL, when the barrier three lines away
        // already does the work: no validation layer in this tree can see the
        // rule it guards. VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 is
        // checked against the per-command-buffer image-layout map of the
        // command buffer the encode is recorded into, and the staging
        // barriers are recorded into a DIFFERENT command buffer
        // (m_inputCommandBufferPool, which is the compute filter itself on a
        // filter-equipped session). The encode command buffer therefore has
        // no map entry for this image and the check silently returns true.
        // That is why CF-02a and CF-02b survived a run that was celebrated as
        // "144 -> 0 validation messages": zero was the right count for a
        // check that never executed.
        //
        // ONE WRITER, ONE READER, deliberately -- the same discipline as
        // VulkanVideoImagePoolNode::SetStagedInputResidualLayout. The writer
        // is the END of each StageInputFrame arm: the arm seeds a running
        // variable with the layout the copy/filter needs and re-reads it
        // from the hand-off TransitionImageLayout's RETURN. Delete the
        // hand-off call and the record keeps saying TRANSFER_DST_OPTIMAL
        // (copy arm) or GENERAL (filter arm), so the reader goes RED. That
        // mutation is how this check was proven able to fail.
        //
        // AND THAT IS THE WHOLE OF WHAT IT PROVES. TransitionImageLayout
        // has exactly ONE return -- an unconditional `return newLayout`, an
        // echo of its own by-value argument, which the body never reassigns
        // -- so this field records the layout the CALL ASKED FOR and never
        // an observation of the barrier. Suppress the CmdPipelineBarrier2KHR
        // inside TransitionImageLayout and this field still reads
        // VIDEO_ENCODE_SRC_KHR. This reader gates the CALL SITE, not the
        // barrier record.
        //
        // VK_IMAGE_LAYOUT_MAX_ENUM means "this frame was not staged", which
        // is the Path-A / RESIDENCY_LOCAL zero-copy case and the AV1
        // show-existing pseudo-frames. Those declare their own layout through
        // VkVideoEncoderExternalImageDescriptor::defaultLayout and are
        // conformant by declaration, so the reader must not judge them.
        VkImageLayout  srcEncodeImageStagedLayout{VK_IMAGE_LAYOUT_MAX_ENUM};

        // ROUTING INTENT for this frame: does its input need the preprocess
        // compute filter to become encodable? Decided by whichever
        // SetExternalInputFrame* entry point admitted the frame -- from the
        // registration's resolved input path on the registered arm, from the
        // frame's own format on the legacy arm -- and read by
        // StageInputFrame. Per FRAME, not per session: with one session,
        // registrations in the session's encode format take the direct path
        // and registrations in its filter-input format take the filter, and
        // the blanket "external input never filters" predicate this replaces
        // could express neither.
        //
        // Never true for a file-input frame; those reach the filter through
        // the session-level enablePreprocessComputeFilter as they always did.
        bool           externalInputViaFilter{false};

        // WHAT ACTUALLY RAN, written by StageInputFrame after it has taken a
        // branch and read by SubmitStagedInputFrame to pick the queue. These
        // two facts must never be inferred from each other: the submit used
        // to key off the filter OBJECT existing, so the moment a session had
        // a filter, EVERY staged frame -- including the ones that took the
        // plain copy, whose queue-family acquire names the transfer/encode
        // family -- was submitted on the compute queue. A queue-family
        // acquire executed on a queue outside its destination family is not a
        // slowdown; it is a wedged queue, and it presents as a hang, so a
        // timing-out test reads as flakiness rather than as this defect.
        bool           inputFilterRecorded{false};

        // The CALLER's frame identifier (VkVideoEncodeInputFrame::
        // frameId), stored verbatim at SetExternalInputFrame() time and used
        // to key the captured-bitstream FIFO (CapturedBitstream::frameId).
        // Previously the capture was keyed by frameEncodeInputOrderNum, the
        // library-internal encode-input counter. The two coincide only while
        // every accepted SubmitExternalFrame() reaches EncodeFrameCommon()
        // exactly once AND there is no B-frame reordering; any partially
        // failed submission (EncodeFrameCommon increments the counter, then
        // EncodeFrame errors and the caller drops the frame) desynchronizes
        // the counter from the caller's ids PERMANENTLY, after which every
        // capture is routed to the wrong (or no) pending frame. uint64_t(-1)
        // == "not an external frame" (file-based path) -- the capture then
        // falls back to frameEncodeInputOrderNum.
        uint64_t       externalFrameId{uint64_t(-1)};

        // Caller-requested mid-stream IDR (VkVideoEncodeInputFrame::
        // forceIDR). Consumed by EncodeFrameCommon(), which passes it as
        // GetPositionInGOP()'s "start a new IDR sequence" trigger -- the
        // SAME path a periodic idrPeriod boundary takes, so the forced
        // frame gets FRAME_TYPE_IDR, resets the GOP state machine (the GOP
        // cadence restarts at this frame, matching VAAPI keyframe
        // semantics), flushes the deferred queue and flows through the
        // codec's normal IDR handling (idr_pic_id, DPB flush, header
        // emission).
        bool           forceIdrOnInput{false};

        // Caller-requested per-frame quantizer (VkVideoEncodeInputFrame::
        // qpOverride), -1 for "use the session's configured constQp".
        //
        // Consumed by EncodeFrameCommon(), which applies it AFTER copying
        // the config's constQp -- that copy is unconditional, so a value
        // written anywhere earlier would be silently overwritten.
        //
        // Units are the codec's own QP units, the same ones the config's
        // constQp uses: 0..51 for H.264/H.265, qindex 0..255 for AV1. There
        // is no second convention to learn.
        //
        // Honoured ONLY when the session's rate-control mode is DISABLED.
        // In CBR/VBR the encoder owns QP and a per-frame override would
        // fight its rate controller, so it is refused there rather than
        // half-applied.
        int32_t        qpOverrideOnInput{-1};

        // Caller-declared queue-family ownership of the external
        // input image. Consumed by StageInputFrame() to decide whether the
        // pre-copy barrier is a FOREIGN_EXT -> staging-family acquire (QFOT)
        // or a plain (HOST-stage-legal) transition. AUTO falls back to the
        // legacy "PREINITIALIZED means local" layout heuristic, which is
        // wrong for REUSED local staging images (their true layout after the
        // first staging copy is TRANSFER_SRC_OPTIMAL).
        ExternalInputResidency externalInputResidency{EXTERNAL_INPUT_RESIDENCY_AUTO};

        // Wait semaphores: the staging/encode command buffer will wait on these
        // before accessing the external input image. Typically this is the
        // producer's graph timeline semaphore.
        std::vector<VkSemaphore>              inputWaitSemaphores;
        std::vector<uint64_t>                 inputWaitSemaphoreValues;  // 0 for binary
        std::vector<VkPipelineStageFlags2>    inputWaitDstStageMasks;

        // Signal semaphores: signaled when the staging copy is complete and the
        // external input image is no longer needed. Typically this is the
        // consumer's release timeline semaphore.
        std::vector<VkSemaphore>              inputSignalSemaphores;
        std::vector<uint64_t>                 inputSignalSemaphoreValues;  // 0 for binary

        void ClearExternalInputSync() {
            isExternalInput = false;
            srcExternalLayoutIsExplicit = false;
            srcEncodeImageIsExternal = false;
            // Cleared with the rest of the per-frame input state, so a pool
            // node recycled from a staged frame into a non-staged role (a
            // Path-A frame, an AV1 show-existing pseudo-frame) cannot carry
            // the previous tenant's record into a frame that never staged.
            srcEncodeImageStagedLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
            externalInputViaFilter = false;
            inputFilterRecorded = false;
            externalFrameId = uint64_t(-1);
            forceIdrOnInput = false;
            qpOverrideOnInput = -1;
            externalInputResidency = EXTERNAL_INPUT_RESIDENCY_AUTO;
            inputWaitSemaphores.clear();
            inputWaitSemaphoreValues.clear();
            inputWaitDstStageMasks.clear();
            inputSignalSemaphores.clear();
            inputSignalSemaphoreValues.clear();
        }

        VkResult SyncHostOnCmdBuffComplete() {

            if (inputCmdBuffer) {
                inputCmdBuffer->ResetCommandBuffer(true, "encoderStagedInputFence");
            }

            if (qpMapCmdBuffer) {
                qpMapCmdBuffer->ResetCommandBuffer(true, "encoderStagedQpMapFence");
            }

            if (encodeCmdBuffer) {
                encodeCmdBuffer->ResetCommandBuffer(true, "encoderEncodeFence");
            }

            return VK_SUCCESS;
        }

        static void ReleaseChildrenFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& dependantFrames) {

            if (dependantFrames == nullptr) {
                // Base case: if frame is null, do nothing
                return;
            }

            // Recursive case: process the next frame first
            ReleaseChildrenFrames(dependantFrames->dependantFrames);

            // After processing the next frame, reset the current frame
            dependantFrames = nullptr;
        }

        // Releases a deferred-frame chain when the frames are NOT handed to
        // the async-assembly queue.  Pool-recycled frame nodes do not run
        // destructors when their last reference drops, so each frame's
        // resources (DPB setup image, input image, bitstream buffer, command
        // buffers) must be dropped explicitly via Reset() - mirroring
        // ReleaseAssemblyItem on the async path.  ReleaseChildrenFrames alone
        // leaves those resources pinned inside the pooled nodes and starves
        // the image/buffer pools (--syncAssembly: DPB image pool exhausted
        // after ~16 frames, truncating the output).
        static void ResetAndReleaseFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames) {
            VkSharedBaseObj<VkVideoEncodeFrameInfo> frame = frames;
            frames = nullptr;
            while (frame != nullptr) {
                VkSharedBaseObj<VkVideoEncodeFrameInfo> next = frame->dependantFrames;
                frame->dependantFrames = nullptr;
                frame->Reset(true);
                frame = next;
            }
        }

        template <typename Callback>
        static VkResult ProcessFrames(VkVideoEncoder* encoder,
                                      VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame,
                                      uint32_t& processFramesIndex,
                                      uint32_t  totalFrameCount,
                                      Callback callback)
        {
            if (frame == nullptr) {
                return VK_SUCCESS;  // Base case: No more frames, return success
            }

            assert(processFramesIndex < totalFrameCount);

            // Invoke the callback for the current frame
            VkResult result = callback(frame, processFramesIndex, totalFrameCount);
            if (result != VK_SUCCESS) {
                return result;  // If an error occurred, return the error code
            }

            // Increment the counter after processing the current frame
            processFramesIndex++;

            // Recursive call to process the next frame
            return ProcessFrames(encoder, frame->dependantFrames,
                                 processFramesIndex, totalFrameCount, callback);
        }

        template <typename Callback>
        static VkResult ProcessFramesReverse(VkVideoEncoder* encoder,
                                             VkSharedBaseObj<VkVideoEncodeFrameInfo>& frame,
                                             uint32_t& lastFramesIndex,
                                             uint32_t  totalFrameCount,
                                             Callback callback)
        {
            if (frame == nullptr) {
                return VK_SUCCESS;  // Base case: No more frames, return success
            }

            // Recursive call to process the next frame
            VkResult result = ProcessFramesReverse(encoder, frame->dependantFrames,
                                                   lastFramesIndex, totalFrameCount, callback);

            if (result != VK_SUCCESS) {
                return result;  // If an error occurred, return the error code
            }

            // Decrement the counter before processing the next frame
            lastFramesIndex--;

            assert(lastFramesIndex < totalFrameCount);

            // Invoke the callback for the current frame at the end
            return callback(frame, lastFramesIndex, totalFrameCount);
        }

        virtual void Reset(bool releaseResources = true) {
            // Clear and check state
            assert(encodeInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR);

            encodeInfo.pNext = nullptr;

            if ((frameInputOrderNum == (uint64_t)-1) &&
                (frameEncodeInputOrderNum == (uint64_t)-1) &&
                (frameEncodeEncodeOrderNum == (uint64_t)-1)) {
                // it is already reset
                return;
            }

            frameInputOrderNum = (uint64_t)-1;          // For debugging
            frameEncodeInputOrderNum = (uint64_t)-1;    // For debugging
            frameEncodeEncodeOrderNum = (uint64_t)-1;   // For debugging
            gopPosition.inputOrder  = uint32_t(-1);     // For debugging
            gopPosition.encodeOrder = uint32_t(-1);     // For debugging
            picOrderCntVal = -1; // For debugging
            gopPosition.pictureType = VkVideoGopStructure::FRAME_TYPE_INVALID;
            inputTimeStamp = (uint64_t)-1; // For debugging
            bitstreamHeaderBufferSize = 0;
            bitstreamHeaderOffset = 0;
            qualityLevel = 0;
            islongTermReference = false;
            sendControlCmd = false;
            sendResetControlCmd = false;
            sendQualityLevelCmd = false;
            sendRateControlCmd = false;
            lastFrame = false;
            controlCmd = VkVideoCodingControlFlagsKHR();
            pControlCmdChain = nullptr;
            // Pool nodes are recycled into non-external roles (e.g. AV1
            // show-existing pseudo-frames); stale external-input state would
            // make them inject stale release-semaphore signals and defeat
            // the flush-point (last-external-frame) detection.
            ClearExternalInputSync();
            assert(qualityLevelInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR);
            assert(rateControlInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR);
            assert(rateControlLayersInfo[0].sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR);
            assert(referenceSlotsInfo[0].sType == VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
            assert(setupReferenceSlotInfo.sType == VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);

            // Clear up the resources
            if (releaseResources) {

                SyncHostOnCmdBuffComplete();

                videoSession = nullptr;
                videoSessionParameters = nullptr;
                srcStagingImageView = nullptr;
                srcEncodeImageResource = nullptr;
                setupImageResource = nullptr;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
                subsampledImageResource = nullptr;  // Release subsampled Y image back to pool
#endif // NV_AQ_GPU_LIB_SUPPORTED
                outputBitstreamBuffer = nullptr;
                assert(numDpbImageResources <= ARRAYSIZE(dpbImageResources));
                for (uint32_t i = 0; i < numDpbImageResources; i++) {
                   dpbImageResources[i] = nullptr;
                }
                numDpbImageResources = 0;
                inputCmdBuffer = nullptr;
                qpMapCmdBuffer = nullptr;
                encodeCmdBuffer = nullptr;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
                aqProcessorSlot = nullptr;
#endif // NV_AQ_GPU_LIB_SUPPORTED
                // recurse and free the children frames
                ReleaseChildrenFrames(dependantFrames);
            }
        }

        virtual ~VkVideoEncodeFrameInfo() {
        }

        void Init() {
            Reset();
        }

        void Deinit() {
            Reset();
        }

        VkResult SetParent(VkSharedBaseObj<VulkanBufferPoolIf> buffPool, int32_t parentIndex)
        {
            assert(m_parent == nullptr);
            m_parent      = std::move(buffPool);
            assert(m_parentIndex == -1);
            m_parentIndex = parentIndex;

            return VK_SUCCESS;
        }

        void ClearParent()
        {
            m_parentIndex = -1;
            m_parent = nullptr;
        }

    private:
        VkSharedBaseObj<VulkanBufferPoolIf>  m_parent;
        int32_t                             m_parentIndex;
        VkVideoCodecOperationFlagBitsKHR    m_codec;
    };
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    class DisplayQueue {

    public:
        DisplayQueue()
        : m_displayShell()
        , m_videoDispayQueue()
        , m_runLoopThread() {}

        VkResult AttachDisplayQueue(VkSharedBaseObj<Shell>& displayShell,
                                    VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>>& videoDispayQueue,
                                    bool runDisplayQueue = true)
        {
            m_displayShell = displayShell;
            m_videoDispayQueue = videoDispayQueue;

            // Run the display queue if it is enabled
            if (runDisplayQueue) {
                Run();
            }
            return VK_SUCCESS;
        }

        bool Run() {
            if (m_displayShell) {
                 // Create and detach the thread
                m_runLoopThread = std::thread(&Shell::RunLoop, m_displayShell);
             }
            return true;
        }

        void Flush() {

            if (m_videoDispayQueue) {
                m_videoDispayQueue->StopQueue();
                m_displayShell->QuitLoop();
                if (m_runLoopThread.joinable()) {
                    // m_runLoopThread.join();
                }
                m_displayShell = nullptr;
                m_videoDispayQueue = nullptr;
            }
        }

        ~DisplayQueue()
        {
            Flush();
        }

        int32_t EnqueueFrame(VulkanEncoderInputFrame* pFrame)
        {
            return m_videoDispayQueue->EnqueueFrame(pFrame);
        }

        bool IsValid()
        {
            return ((m_displayShell != nullptr) && (m_videoDispayQueue != nullptr));
        }

    private:
        VkSharedBaseObj<Shell> m_displayShell;
        VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>> m_videoDispayQueue;
        std::thread            m_runLoopThread;
    };
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
public:
    VkVideoEncoder(const VulkanDeviceContext* vkDevCtx)
        : m_encoderConfig()
        , m_vkDevCtx(vkDevCtx)
        , m_inputFrameNum(0)
        , m_encodeInputFrameNum(0)
        , m_encodeEncodeFrameNum(0)
        , m_videoSession()
        , m_videoSessionParameters()
        , m_imageDpbFormat()
        , m_imageInFormat()
        , m_maxCodedExtent()
        , m_maxDpbPicturesCount(16)
        , m_minStreamBufferSize(2 * 1024 * 1024)
        , m_streamBufferSize(m_minStreamBufferSize)
        , m_rateControlInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR }
        , m_rateControlLayersInfo{{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR }}
        , m_picIdxToDpb{}
        , m_gopState()
        , m_dpbSlotsMask(0)
        , m_frameNumSyntax(0)
        , m_frameNumInGop()
        , m_IDRPicId(0)
        , m_videoMaintenance1FeaturesSupported(false)
        , m_sendControlCmd(true)
        , m_sendResetControlCmd(true)
        , m_sendQualityLevelCmd(true)
        , m_sendRateControlCmd(true)
        , m_useImageArray(false)
        , m_useImageViewArray(false)
        , m_useSeparateOutputImages(false)
        , m_useLinearInput(false)
        , m_resetEncoder(false)
        , m_enableEncoderThreadQueue(false)
        , m_verbose(false)
        , m_numDeferredFrames()
        , m_numDeferredRefFrames()
        , m_holdRefFramesInQueue(1)
        , m_maxSubmittedInputReleaseId(0)
        , m_lastSignaledInputReleaseId(0)
        , m_controlCmd(VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR |
                       VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR |
                       VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR)
        , m_linearInputImagePool()
        , m_inputImagePool()
        , m_dpbImagePool()
        , m_inputCommandBufferPool()
        , m_encodeCommandBufferPool()
        , m_bitstreamBuffersQueue()
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
        , m_displayQueue()
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
        , m_hwLoadBalancingTimelineSemaphore()
        , m_currentVideoQueueIndx(-1)
        , m_completionTimelineSemaphore()
        , m_completionSemaphoreExportable(false)
        , m_maxSubmittedCompletionValue(0)
        , m_lastSignaledCompletionValue(0)
        , m_imageQpMapFormat()
        , m_qpMapTexelSize()
        , m_qpMapTiling()
        , m_linearQpMapImagePool()
        , m_qpMapImagePool()
        , m_psnr()
    { }

    // Factory Function
    static VkResult CreateVideoEncoder(const VulkanDeviceContext* vkDevCtx,
                                       VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                       VkSharedBaseObj<VkVideoEncoder>& encoder);

    virtual VkVideoEncoderH264* GetVideoEncoderH264() {
        return nullptr;
    }

    virtual VkVideoEncoderH265* GetVideoEncoderH265() {
        return nullptr;
    }

#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    VkResult AttachDisplayQueue(VkSharedBaseObj<Shell>& displayShell,
                                VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>>& videoDispayQueue)
    {
        return m_displayQueue.AttachDisplayQueue(displayShell, videoDispayQueue);
    }
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT

    virtual VkResult CreateFrameInfoBuffersQueue(uint32_t numPoolNodes) = 0;
    // True when SubmitExternalFrame can accept another input frame
    // without the submit path blocking on the assembly queue's producer
    // condition variable and without unbounded captured-bitstream
    // growth. Submission is session-serial, so between this check and
    // the enqueue the queue can only drain; the check is conservative.
    bool CanAcceptNewInputFrame() const;

    // Upper bound on the number of items ONE EnqueueFrame() can push into
    // m_assemblyQueue. The chain at rest is a run of non-reference frames
    // (postFlushQueue fires on every reference frame, and both counters reset
    // on every flush), so the bound is that run plus the reference frame whose
    // insertion flushes it. Reads the generator's sub-GOP CYCLE, not the
    // configured B count: the two can disagree, and the cycle is what places
    // reference frames.
    virtual size_t GetMaxAssemblyBurst() const {
        if (!m_encoderConfig) { return 1u; }
        const uint8_t cycle = m_encoderConfig->gopStructure.GetGopFrameCycle();
        return (size_t)((cycle > 0) ? cycle : 1u);
    }

    // Create the completion timeline semaphore (ext currency 3; idempotent).
    // Called by the ext layer at initialization, on the session-serial
    // thread, before any submit. Exportability is a physical-device
    // property, queried rather than assumed; when the answer is no, the
    // semaphore is created plain and export is refused by type at the ext
    // layer.
    VkResult CreateCompletionTimelineSemaphore();
    VkSemaphore GetCompletionTimelineSemaphore() const {
        return m_completionTimelineSemaphore;
    }
    bool IsCompletionSemaphoreExportable() const {
        return m_completionSemaphoreExportable;
    }

    virtual bool GetAvailablePoolNode(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) = 0;

    virtual VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig) = 0; // Must be implemented by the codec

    // === File-based input path (existing) ===
    VkResult LoadNextFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult LoadNextQpMapFrameFromFile(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);

    // === External frame input path (new) ===
    // Replaces LoadNextFrame() for externally-provided images.
    // Sets up the encodeFrameInfo with the external image, bookkeeping fields,
    // and sync semaphores, then calls StageInputFrame() which chains to
    // EncodeFrameCommon(). All paths route through StageInputFrame for DPB safety.
    //
    // externalImage: VkImage already on the same device (e.g. imported from DMA-BUF)
    // externalMemory: VkDeviceMemory backing the image (can be VK_NULL_HANDLE for non-owning)
    // format, width, height: image properties
    // tiling: VK_IMAGE_TILING_OPTIMAL or VK_IMAGE_TILING_LINEAR
    // frameId: unique frame identifier (passed through to output)
    // pts: presentation timestamp
    // isLastFrame: set true for the final frame (triggers EOS)
    // forceIdr: encode this frame as an IDR and restart the GOP
    //           sequence at it (mid-stream keyframe request)
    // qpOverride: per-frame quantizer in the codec's own units, -1 for
    //           none. Honoured only when rate control is DISABLED.
    // residency: queue-family ownership of the input image
    //            (AUTO = legacy layout heuristic)
    // waitSemaphore/signalSemaphore arrays: injected into SubmitStagedInputFrame
    VkResult SetExternalInputFrame(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
        VkImage externalImage,
        VkDeviceMemory externalMemory,
        VkFormat format,
        uint32_t width, uint32_t height,
        VkImageTiling tiling,
        VkImageLayout srcImageCurrentLayout,
        uint64_t frameId,
        uint64_t pts,
        bool isLastFrame,
        bool forceIdr,
        int32_t qpOverride,
        ExternalInputResidency residency,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        const VkPipelineStageFlags2* pWaitDstStageMasks,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // Registered-path twin of SetExternalInputFrame: the wrap already
    // happened, once, at registration (see the ext layer's
    // BuildRegisteredViewLocked). Performs the same bookkeeping and routing
    // but creates NO Vulkan object and NO wrapper: |node| is shared by
    // every frame of its registration and is read-only on the encode path
    // (GetPictureResourceInfo / GetImageView are the only consumers).
    // |directlyEncodable| is the registration-time routing predicate
    // (encodable format, non-LINEAR tiling, encode-capable usage),
    // replacing the legacy arm's per-frame tiling check.
    VkResult SetExternalInputFrameWithNode(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
        VkSharedBaseObj<VulkanVideoImagePoolNode>& node,
        // The registration id this frame names, purely so the staging arm can
        // tell the content probe WHICH BUFFER it is looking at. 0 means "not
        // a registered submit", which the probe treats as never-armed.
        uint64_t registrationId,
        bool directlyEncodable,
        // The registration's resolved rung of the adaptation ladder, for the
        // frames |directlyEncodable| refuses: true routes the staged frame
        // through the preprocess compute filter, false through the transfer
        // copy. Decided once at registration by the layer that knows the
        // descriptor's format and the session's, and passed down rather than
        // re-derived here, so routing and the registration's reported input
        // path cannot disagree.
        bool routeViaFilter,
        VkImageLayout srcImageCurrentLayout,
        // True when |srcImageCurrentLayout| came from the FRAME's own
        // currentLayout field and false when it is the registration's
        // defaultLayout standing in. Only the ext layer can tell those
        // apart -- it is the one that applies the UNDEFINED sentinel -- so
        // it is passed down rather than re-derived. Decides whether the
        // staged-input acquire may prefer the library's own residual
        // record over the declaration.
        bool srcLayoutIsExplicit,
        uint64_t frameId,
        uint64_t pts,
        bool isLastFrame,
        bool forceIdr,
        int32_t qpOverride,
        ExternalInputResidency residency,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        const VkPipelineStageFlags2* pWaitDstStageMasks,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // Shared bookkeeping of the two external-input entry points (the
    // "replicate LoadNextFrame" and "store external sync info" sections),
    // factored so the legacy and registered arms cannot drift.
    void StampExternalFrameInfo(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
        VkImageLayout srcImageCurrentLayout,
        uint64_t frameId,
        uint64_t pts,
        bool isLastFrame,
        bool forceIdr,
        int32_t qpOverride,
        ExternalInputResidency residency,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        const VkPipelineStageFlags2* pWaitDstStageMasks,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // Helper: wrap an external VkImage as a VulkanVideoImagePoolNode.
    // Legacy per-frame wrap for SubmitExternalFrame only; registered
    // submissions carry a node built once at registration. Slated for
    // removal with the M5 consumer migration.
    VkResult WrapExternalImage(
        VkImage image, VkDeviceMemory memory,
        VkFormat format, uint32_t width, uint32_t height,
        VkImageTiling tiling,
        VkSharedBaseObj<VulkanVideoImagePoolNode>& outNode);

    VkResult StageInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult StageInputFrameQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                  VkCommandBuffer cmdBuf = VK_NULL_HANDLE);
    VkResult SubmitStagedInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult SubmitStagedQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult EncodeFrameCommon(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) = 0; // Must be implemented by the codec
    virtual bool HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);

    virtual VkResult CodecHandleRateControlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) = 0; // Must be implemented by the codec

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    /**
     * @brief Gets the subsampled Y image pool
     *
     * Provides direct access to the subsampled Y image pool for AQ operations.
     * Encoder manages this pool like other image pools.
     *
     * @return Shared pointer to the subsampled Y image pool, or nullptr if not allocated
     */
    VkSharedBaseObj<VulkanVideoImagePool> GetSubsampledYImagePool() const {
        return m_inputSubsampledImagePool;
    }
#endif // NV_AQ_GPU_LIB_SUPPORTED

    virtual VkResult RecordVideoCodingCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                         uint32_t frameIdx, uint32_t ofTotalFrames);

    VkResult RecordVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, uint32_t numFrames);

    // Capacity of the DIRECT (zero-copy) submit's wait and signal arrays.
    //
    // SubmitVideoCodingCmds assembles that submit into fixed stack arrays of
    // this many entries, and a frame whose assembled list would not fit is
    // REFUSED with VK_ERROR_TOO_MANY_OBJECTS rather than submitted with an
    // entry dropped: a dropped wait lets the encode read the input image
    // while the producer named by that wait is still writing it, and a
    // dropped signal is a semaphore nobody ever signals. The staged path
    // assembles into a growable vector and carries no such bound.
    //
    // One number, named once, because the entry points that refuse an
    // over-capacity frame early -- while a status can still be returned to
    // the caller -- have to refuse at exactly the count the arrays hold. A
    // second literal that drifted from this one would reopen the gap it
    // exists to close.
    static constexpr uint32_t kDirectSubmitSemaphoreCapacity = 8;

    virtual VkResult SubmitVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                           uint32_t frameIdx, uint32_t ofTotalFrames);

    virtual VkResult AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                           uint32_t frameIdx, uint32_t ofTotalFrames);

    
    // Returns BYTES ACCOUNTED FOR, which is not the same as bytes fwritten.
    //
    // When disableFileOutput is set the fwrite is skipped and this returns
    // `size` anyway. That is deliberate and load-bearing, not an oversight:
    // both loop-driving callers -- VkVideoEncoder::WriteBitstreamToFileOutput()
    // and VkVideoEncoderAV1::FlushBatchedTemporalUnit() -- drive a partial-write
    // loop
    //     while (written < total) { n = WriteDataToFile(...); if (!n) fail; }
    // and read 0 as a hard failure. Returning 0 under the suppression flag
    // therefore turns the CLI's own documented discard mode
    // (--disableFileOutput, "suppress ALL bitstream file output", see
    // VkEncoderConfig.cpp) into an error: every frame would report "Error
    // writing VCL data" and AssembleBitstreamData would return
    // VK_ERROR_OUT_OF_HOST_MEMORY, while the run still exited 0.
    //
    // So 0 means "the write was attempted and came up short" and nothing
    // else. Callers must not read a non-zero return as evidence that bytes
    // reached a file; in capture mode they reach m_capturedBitstreams
    // instead, and under `--syncAssembly --disableFileOutput` they reach
    // neither -- that combination is encode-and-discard by construction,
    // which is what those two flags jointly ask for.
    size_t WriteDataToFile(const uint8_t* data, size_t size);

private:
    // File-output arm of WriteBitstreamToFile: writes the non-VCL header, then
    // the coded payload described by readback, which ReadbackBitstreamData()
    // has already fetched from the feedback query pool.
    // Private and non-virtual: it must never grow a second completion publish.
    VkResult WriteBitstreamToFileOutput(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                        BitstreamReadback& readback);

public:
    virtual VkResult ReadbackBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                           BitstreamReadback& readback);

    virtual VkResult WriteBitstreamToFile(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                          uint32_t frameIdx, uint32_t ofTotalFrames,
                                          BitstreamReadback& readback);

    /**
     * @brief Get the CRC values for the encoded data
     *
     * @param pCrcValues Pointer to store the CRC values
     * @param buffSize Size of the buffer to store the CRC values
     * @return size_t Number of CRC values written, (size_t)-1 on error
     */
    virtual size_t GetCrcValues(uint32_t* pCrcValues, size_t buffSize) const;

    /**
     * @brief Get the average PSNR (dB) for the encoded stream (input vs reconstructed frames).
     * @return Average PSNR in dB, or -1.0 if PSNR was not enabled or no frames were measured.
     */
    virtual double GetAveragePsnr() const;
    /** Average chroma PSNR (dB), or -1.0 if not applicable / disabled. */
    virtual double GetAveragePsnrU() const;
    virtual double GetAveragePsnrV() const;

    virtual VkResult StartOfVideoCodingEncodeOrder(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, uint32_t frameIdx, uint32_t ofTotalFrames)
    {
        encodeFrameInfo->frameEncodeEncodeOrderNum = m_encodeEncodeFrameNum++;
        if (m_encoderConfig->verboseFrameStruct) {
            DumpStateInfo("start encoding", 2, encodeFrameInfo, frameIdx, ofTotalFrames);
        }
        return VK_SUCCESS;
    }

    virtual VkResult InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp) = 0; // Must be implemented by the codec

    const uint8_t* setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset);

    /**
     * @brief Copies YCbCr planes directly from input buffer to output buffer when formats are the same
     *
     * @param pInputFrameData Source buffer containing YCbCr planes
     * @param inputPlaneLayouts Array of source buffer plane layouts (offset, pitch, etc.)
     * @param writeImagePtr Destination buffer for the YCbCr planes
     * @param dstSubresourceLayout Array of destination buffer plane layouts
     * @param width Width of the image in pixels
     * @param height Height of the image in pixels
     * @param numPlanes Number of planes in the format (1, 2, or 3)
     * @param format The VkFormat of the image for proper subsampling and bit depth detection
     * @return none
     */
    void CopyYCbCrPlanesDirectCPU(
        const uint8_t* pInputFrameData,
        const VkSubresourceLayout* inputPlaneLayouts,
        uint8_t* writeImagePtr,
        const VkSubresourceLayout* dstSubresourceLayout,
        uint32_t width,
        uint32_t height,
        uint32_t numPlanes,
        VkFormat format);

    // Drain the pipeline and join every worker. Returns true when the
    // session completed everything it was given, and FALSE when it did not --
    // a frame the encoder thread could not process, a bitstream the assembly
    // workers could not read back or write, or a deferred frame that could
    // not be pushed. The threads report each failure as it happens, but a
    // process that only ever sees the end of the run has no other place to
    // learn that one occurred, and a bitstream is not evidence: a session
    // that failed on its first frame leaves a file of zero bytes behind.
    bool WaitForThreadsToComplete();
    // Queue a mid-stream rate-control update. Thread-safe
    // producer; applied on the encoder thread at the next frame boundary.
    VkResult RequestRateControlUpdate(uint64_t averageBitrate,
                                      uint64_t maxBitrate,
                                      uint32_t frameRateNumerator,
                                      uint32_t frameRateDenominator);

protected:

    // Called by the InitEncoderCodec to initialize the common encoder code.
    VkResult InitEncoder(VkSharedBaseObj<EncoderConfig>& encoderConfig);

    VkDeviceSize GetBitstreamBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer);

    // Local patch; not in upstream vk_video_samples. Optional queue-family-
    // ownership transfer for imported (VK_QUEUE_FAMILY_FOREIGN_EXT)
    // external images.
    VkImageLayout TransitionImageLayout(VkCommandBuffer cmdBuf,
                                        VkSharedBaseObj<VkImageResourceView>& imageView,
                                        VkImageLayout oldLayout, VkImageLayout newLayout,
                                        uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);

    // Local patch; not in upstream vk_video_samples.
    //
    // The RELEASE half of the FOREIGN_EXT ownership transfers the acquire
    // sites perform. Deliberately NOT TransitionImageLayout:
    //
    //  (a) That function picks its four barrier masks from an
    //      (oldLayout,newLayout) if/else chain, and an unhandled pair still
    //      gets a barrier -- now a deliberate ALL_COMMANDS/MEMORY_* fallback
    //      with a loud diagnostic, rather than the struct defaults.
    //
    //      THIS SUB-ARGUMENT USED TO REST ON A BUILD DIVERGENCE THAT NO
    //      LONGER EXISTS, and is corrected here rather than quietly left to
    //      rot. The chain's final else was
    //      `#ifdef __cpp_exceptions throw ... #endif`, which meant the
    //      standalone CMake build (exceptions ON) terminated on an unhandled
    //      pair while Chromium's -fno-exceptions build silently kept the
    //      defaults -- so a ctest could not reproduce the shipping failure
    //      mode. Both halves were replaced by one total fallback that behaves
    //      identically in both builds, so that particular reason to avoid
    //      this table is gone.
    //
    //      What survives, and is why this helper still exists: the fallback
    //      is correct-but-conservative for an ACQUIRE and still not right for
    //      a RELEASE, for reason (b) below.
    //  (b) For a RELEASE the spec USES srcStageMask/srcAccessMask and
    //      IGNORES the dst pair -- the exact inverse of an acquire. So the
    //      defaults that let an unhandled ACQUIRE survive by luck turn an
    //      unhandled RELEASE into an ownership transfer with an EMPTY first
    //      synchronisation scope: ordered against nothing, and invisible to
    //      the validation layers, since VK_PIPELINE_STAGE_2_NONE is
    //      trivially queue-valid.
    //  (c) The table dispatches on the layout pair alone and ignores the
    //      queue-family arguments, so one table cannot serve both
    //      directions regardless.
    //
    // The caller therefore states the src masks explicitly.
    // |oldLayout| must be the layout our last use actually left the image in
    // (VUID-VkImageMemoryBarrier2-oldLayout-01197). |newLayout| is the layout
    // the FOREIGN consumer will find it in; it must not be UNDEFINED or
    // PREINITIALIZED (VUID-...-newLayout-01198).
    void ReleaseImageToForeignQueue(VkCommandBuffer cmdBuf,
                                    VkSharedBaseObj<VkImageResourceView>& imageView,
                                    VkImageLayout oldLayout,
                                    VkImageLayout newLayout,
                                    uint32_t srcQueueFamilyIndex,
                                    VkPipelineStageFlags2KHR srcStageMask,
                                    VkAccessFlags2KHR srcAccessMask);

    // Local patch; not in upstream vk_video_samples.
    //
    // The LOCAL-residency counterpart of ReleaseImageToForeignQueue: it hands
    // a staged input image back in a layout the NEXT frame's acquire can name
    // truthfully, and RETURNS that layout so the caller can record it.
    //
    // WHY THIS EXISTS. A registration that is reused across frames is
    // declared once, and StageInputFrame records that declaration as its
    // acquire oldLayout. Nothing restored the image afterwards unless it was
    // a FOREIGN import, so for a LOCAL registration the declaration was true
    // on frame 1 only: the copy arm leaves the image in TRANSFER_SRC_OPTIMAL
    // and the filter arm leaves it in GENERAL, and every later frame asserted
    // a layout the image was not in
    // (VUID-VkImageMemoryBarrier2-oldLayout-01197).
    //
    // NOT TransitionImageLayout: the destination here comes from the CALLER,
    // so routing it through a table keyed on the layout pair would turn an
    // unusual but legal declaration into the table's terminal throw. This
    // helper is total.
    //
    // THE UNRESTORABLE DECLARATION IS SUBSTITUTED, NOT SKIPPED, and that is a
    // reversal of this helper's original contract. UNDEFINED and
    // PREINITIALIZED are not legal barrier DESTINATIONS
    // (VUID-VkImageMemoryBarrier2-newLayout-01198), and PREINITIALIZED is
    // unrestorable BY CONSTRUCTION -- it means "never yet in any other layout
    // since creation", which can be true at most once in an image's life. So
    // this helper hands such a registration back in VK_IMAGE_LAYOUT_GENERAL:
    // the only other layout in which host access to a LINEAR image is
    // defined, and a legal barrier destination.
    //
    // The original contract recorded NOTHING in that case, on the ground that
    // substituting GENERAL would "replace one false declaration with a
    // different false declaration". That objection was correct while the
    // library kept USING the caller's declaration as the next acquire's
    // oldLayout, and it is dissolved now that it does not: StageInputFrame
    // stores this function's RETURN VALUE on the registration's pool node and
    // names it -- not the declaration -- on the next acquire. The library no
    // longer trusts the declaration; it makes its own statement true. Nothing
    // false is left over for the substitution to add to.
    //
    // WHAT THE CALLER LOSES, stated plainly: a caller that declared
    // PREINITIALIZED and reused the registration finds its image in GENERAL
    // from the second frame on rather than in a layout it named. It could not
    // have been left in PREINITIALIZED by any legal barrier, so there is no
    // behaviour it could have relied on -- and GENERAL is strictly better for
    // the only thing such a caller does between frames, which is host-write a
    // LINEAR image through a persistent mapping, which
    // TRANSFER_SRC_OPTIMAL does not permit at all.
    //
    // NO LAYOUT TRACKING THROUGH m_currentImageLayout. |residualLayout| is
    // still a LITERAL supplied by whichever arm recorded the work -- the same
    // literal that arm named as its acquire newLayout -- never a stored
    // field. The node field this function's return value feeds
    // (m_stagedInputResidualLayout) is a DIFFERENT field from
    // m_currentImageLayout, with one writer and one reader; reading
    // m_currentImageLayout, which three unrelated producers write, is what
    // forced the previous attempt at this fix to be reverted.
    //
    // ONE CASE STILL RECORDS NOTHING: residualLayout already equals the
    // (possibly substituted) target, so our arm left the image exactly where
    // the next acquire will name it. This is what keeps an (X -> X) pair,
    // which the layout table has no arm for, from ever being constructed,
    // without consulting any state. The return value is the target either
    // way, so the caller's record is correct in both.
    //
    // Returns the layout the image is left in.
    VkImageLayout RestoreStagedInputLayout(VkCommandBuffer cmdBuf,
                                  VkSharedBaseObj<VkImageResourceView>& imageView,
                                  VkImageLayout residualLayout,
                                  VkImageLayout declaredLayout,
                                  VkPipelineStageFlags2KHR srcStageMask,
                                  VkAccessFlags2KHR srcAccessMask);


    VkResult CopyLinearToOptimalImage(VkCommandBuffer& commandBuffer,
                                      VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                      VkSharedBaseObj<VkImageResourceView>& dstImageView,
                                      const VkExtent2D& copyImageExtent,
                                      uint32_t srcCopyArrayLayer = 0,
                                      uint32_t dstCopyArrayLayer = 0,
                                      VkImageLayout srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VkImageLayout dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkResult CopyLinearToLinearImage(VkCommandBuffer& commandBuffer,
                                     VkSharedBaseObj<VkImageResourceView>& srcImageView,
                                     VkSharedBaseObj<VkImageResourceView>& dstImageView,
                                     const VkExtent2D& copyImageExtent,
                                     uint32_t srcCopyArrayLayer = 0,
                                     uint32_t dstCopyArrayLayer = 0,
                                     VkImageLayout srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VkImageLayout dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    void ProcessQpMap(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);

    void FillIntraRefreshInfo(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);

    virtual VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                uint32_t frameIdx, uint32_t ofTotalFrames) = 0;

public:
    virtual ~VkVideoEncoder() {
        DeinitEncoder();
        for (auto& t : m_assemblyThreads) {
            if (t.joinable()) t.join();
        }
        if (m_encoderQueueConsumerThread.joinable()) {
            m_encoderQueueConsumerThread.join();
        }
    }

    int32_t DeinitEncoder();

    // Pop the next completion record (FIFO) from
    // m_capturedBitstreams. Returns true if a record was popped. Used by
    // VulkanVideoEncoderExtImpl to drain the in-memory completion queue
    // in both output modes (bytes are carried only in capture mode).
    // *out_status carries the per-frame result -- VK_SUCCESS for a
    // normal capture; the readback failure code (e.g. VK_INCOMPLETE for a
    // non-COMPLETE query status such as INSUFFICIENT_BITSTREAM_BUFFER_RANGE)
    // for a frame whose assembly failed, with empty bytes.
    bool TryPopCapturedBitstream(uint64_t* out_frame_id,
                                  std::vector<uint8_t>* out_bytes,
                                  bool* out_is_idr,
                                  uint32_t* out_picture_type,
                                  VkResult* out_status);

    // Quiesce the pipeline the way WaitForThreadsToComplete() does -- push
    // the deferred GOP tail, join every worker, so that everything submitted
    // so far is encoded AND has published its completion record -- and then
    // bring the assembly workers back up so the session can keep doing both.
    //
    // This is what a NON-TERMINAL drain has to be. WaitForThreadsToComplete()
    // on its own is terminal for the COMPLETION SURFACE, not for the encoder:
    // it clears m_asyncAssemblyEnabled, ProcessOrderedFrames then falls back
    // to the synchronous AssembleBitstreamData, and that path publishes no
    // CapturedBitstream -- so every later frame encodes correctly and is
    // never reported. Callers that really are tearing down (Flush,
    // Deinitialize) keep calling WaitForThreadsToComplete() directly.
    //
    // Returns false if the workers could not be restarted; the drain itself
    // has still happened.
    bool DrainAndRestartThreads();

    // True when someone has registered for the completion edge (the Ext
    // layer does, at InitializeExt). Used to tell "nobody is listening, so
    // publishing is pointless" apart from "somebody is listening and a
    // dropped record is a contract violation".
    bool HasCompletionSubscriber() const {
        std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
        return (m_onBitstreamCaptured != nullptr);
    }

    // Record this frame into the deferred-GOP queue and flush that queue
    // around it. Returns VK_SUCCESS when every flush this call made
    // succeeded, and otherwise the first failure one reported.
    //
    // A flush is where a frame is recorded and submitted, so a frame whose
    // commands could not be recorded or whose submit was refused fails HERE,
    // on the caller thread. This return is the route by which that failure
    // reaches the caller's own status; the drain at the end of the session
    // reports it too, but only once every remaining frame has been given
    // away.
    VkResult EnqueueFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                          bool isIdrFrame, bool isReferenceFrame) {

        VkResult result = VK_SUCCESS;

        const bool preFlushQueue = isIdrFrame;
        if (preFlushQueue) {
            result = PushOrderedFrames();
        }

        InsertOrdered(encodeFrameInfo, isReferenceFrame);

        // Local patch; not in upstream vk_video_samples.
        // Flush per-frame when there is no B-frame reordering. The deferred-GOP
        // queue is otherwise drained only on lastFrame / IDR-preflush / a full
        // reference window. A Chromium stream never signals lastFrame, and with
        // consecutiveBFrames=0 no reordering is needed, so without this trigger
        // every frame piles into m_lastDeferredFrame, the encode-image pool
        // exhausts (~m_holdRefFramesInQueue), RecordVideoCodingCmd never runs, and
        // the bitstream is empty. With no reordering, input order == encode order,
        // so recording each frame immediately is correct and low-latency.
        const bool noReorderingNeeded =
            (m_encoderConfig->gopStructure.GetConsecutiveBFrameCount() == 0);
        const bool postFlushQueue = (encodeFrameInfo->lastFrame ||
                                        noReorderingNeeded ||
                                        (isReferenceFrame && (m_numDeferredRefFrames == m_holdRefFramesInQueue)));
        if (postFlushQueue) {
            // Both flushes are made whatever the first reported: this frame
            // is in the queue by now, and the queue must not be left holding
            // it because an earlier frame failed. The FIRST failure is the
            // one returned -- it is the one with a cause behind it.
            const VkResult postResult = PushOrderedFrames();
            if (result == VK_SUCCESS) {
                result = postResult;
            }
        }
        return result;
    }

    void ConsumerThread();

    // Insert frames in order from the reference frame first and B frames next in the list.
    // Uses a simple ordering for now where B frame as reference are not supported yet.
    virtual void InsertOrdered(VkSharedBaseObj<VkVideoEncodeFrameInfo>& current,
                               VkSharedBaseObj<VkVideoEncodeFrameInfo>& prev,
                               VkSharedBaseObj<VkVideoEncodeFrameInfo>& node) {

        if ((current == nullptr) || (current->gopPosition.encodeOrder >= node->gopPosition.encodeOrder)) {

            node->dependantFrames = current;

            if (prev != nullptr) {
                // If not inserting at the beginning, link the previous node to the new node
                prev->dependantFrames = node;
            } else {
                // If inserting at the beginning, update the head
                m_lastDeferredFrame = node;
            }

            return;
        }

        // Recursive case: Move to the next node, updating previous node pointer
        InsertOrdered(current->dependantFrames, current, node);
    }

    // Wrapper function to start the recursion
    void InsertOrdered(VkSharedBaseObj<VkVideoEncodeFrameInfo>& dependantFrames, bool isReferenceFrame) {
        m_numDeferredFrames++;
        if (isReferenceFrame) {
            m_numDeferredRefFrames++;
        }
        if (m_lastDeferredFrame == nullptr) {
            m_lastDeferredFrame = dependantFrames;
            return;
        }
        VkSharedBaseObj<VkVideoEncodeFrameInfo> prev;
        InsertOrdered(m_lastDeferredFrame, prev, dependantFrames);
    }

    VkResult PushOrderedFrames();
    VkResult ProcessOrderedFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames);
    VkResult ProcessOutOfOrderFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames);

    void DumpStateInfo(const char* stage, uint32_t ident, VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                       int32_t frameIdx = -1, uint32_t ofTotalFrames = 0) const;

    VkResult SelectDrmFormatModifier(VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                     VkFormat format, VkImageUsageFlags usage,
                                     const VkExtent2D& imageExtent);

    struct AssemblyWorkItem {
        VkSharedBaseObj<VkVideoEncodeFrameInfo> frameInfo;
        uint64_t sequenceNumber;
        BitstreamReadback readback;
    };
    typedef VkThreadSafeQueue<VkSharedBaseObj<VkVideoEncodeFrameInfo>> EncoderFrameQueue;
    typedef VkThreadSafeQueue<AssemblyWorkItem> AssemblyQueue;

    void AssemblyWorkerThread(int threadId);
    VkResult QueueFramesForAssembly(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames);
    void ReleaseAssemblyItem(AssemblyWorkItem& item);

protected:
    VkSharedBaseObj<EncoderConfig>                m_encoderConfig;
    const VulkanDeviceContext*                    m_vkDevCtx;
    uint64_t                                      m_inputFrameNum;
    uint64_t                                      m_encodeInputFrameNum;
    uint64_t                                      m_encodeEncodeFrameNum;
    VkSharedBaseObj<VulkanVideoSession>           m_videoSession;
    VkSharedBaseObj<VulkanVideoSessionParameters> m_videoSessionParameters;
    VkFormat                              m_imageDpbFormat;
    VkFormat                              m_imageInFormat;
    VkExtent2D                            m_maxCodedExtent;
    uint32_t                              m_maxDpbPicturesCount;
    size_t                                m_minStreamBufferSize;
    size_t                                m_streamBufferSize;
    VkVideoEncodeQualityLevelInfoKHR      m_qualityLevelInfo;
    VkVideoEncodeRateControlInfoKHR       m_rateControlInfo;
    VkVideoEncodeRateControlInfoKHR       m_beginRateControlInfo;
    VkVideoEncodeRateControlLayerInfoKHR  m_rateControlLayersInfo[1];
    // Value snapshot backing m_beginRateControlInfo.pLayers: the last
    // COMMANDED layer state, immune to pending-update mutation of
    // m_rateControlLayersInfo between control commands.
    VkVideoEncodeRateControlLayerInfoKHR  m_beginRateControlLayersInfo[1] =
        {{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR }};
    // Codec-specific halves of the cached begin-coding rate-control state.
    // The session state a control command establishes includes the codec RC
    // struct and per-layer codec structs, and
    // VUID-vkCmdBeginVideoCodingKHR-pBeginInfo-08254 requires the begin-info
    // chain to match that state in FULL -- so they are snapshotted together
    // with the base struct and layer values.
    VkVideoEncodeH264RateControlInfoKHR      m_beginRateControlInfoH264{};
    VkVideoEncodeH265RateControlInfoKHR      m_beginRateControlInfoH265{};
    VkVideoEncodeAV1RateControlInfoKHR       m_beginRateControlInfoAV1{};
    VkVideoEncodeH264RateControlLayerInfoKHR m_beginRateControlLayersInfoH264[1] = {};
    VkVideoEncodeH265RateControlLayerInfoKHR m_beginRateControlLayersInfoH265[1] = {};
    VkVideoEncodeAV1RateControlLayerInfoKHR  m_beginRateControlLayersInfoAV1[1] = {};
    // Pending mid-stream rate-control update. Produced by any
    // thread via RequestRateControlUpdate() (the ext Reconfigure entry);
    // applied ON THE ENCODER THREAD by ApplyPendingRateControlUpdate(),
    // which runs immediately before the m_sendRateControlCmd consume in the
    // frame-record path -- the refreshed values ride the next
    // VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL command. Both fields are
    // guarded by m_pendingRateControlMutex.
    struct PendingRateControlUpdate {
        uint64_t averageBitrate;
        uint64_t maxBitrate;
        uint32_t frameRateNumerator;
        uint32_t frameRateDenominator;
    };
    void ApplyPendingRateControlUpdate();
    std::mutex               m_pendingRateControlMutex;
    bool                     m_pendingRateControlArmed = false;
    PendingRateControlUpdate m_pendingRateControlUpdate = {};
    int8_t   m_picIdxToDpb[17]; // MAX_DPB_SLOTS + 1
    VkVideoGopStructure::GopState         m_gopState;
    uint32_t m_dpbSlotsMask;
    uint32_t m_frameNumSyntax;
    uint32_t m_frameNumInGop;
    uint32_t m_IDRPicId;
    uint32_t m_videoMaintenance1FeaturesSupported : 1;
    uint32_t m_sendControlCmd : 1;
    uint32_t m_sendResetControlCmd : 1;
    uint32_t m_sendQualityLevelCmd : 1;
    uint32_t m_sendRateControlCmd : 1;
    uint32_t m_useImageArray : 1;
    uint32_t m_useImageViewArray : 1;
    uint32_t m_useSeparateOutputImages : 1;
    uint32_t m_useLinearInput : 1;
    uint32_t m_resetEncoder : 1;
    uint32_t m_enableEncoderThreadQueue : 1;
    uint32_t m_verbose : 1;
    uint32_t                                 m_numDeferredFrames;
    uint32_t                                 m_numDeferredRefFrames;
    uint32_t                                 m_holdRefFramesInQueue;
    // External-input release signaling at queue flush points (Path A).
    // Encode submits arrive in encode order, which under B-frame GOPs
    // differs from input order, so a frame's own release value must not be
    // signaled on its submit (a reordered reference would release the B
    // inputs it precedes while their encodes still have to read them).
    // The end of each ordered batch (reference frame + its deferred
    // B-frames) is a natural flush point: every input received so far has
    // been encode-submitted, so the batch's last submit signals the max
    // release value seen — one monotonic signal releasing the whole
    // sequence, valid for any intra-batch order. Single-frame batches
    // (no B-frames) degenerate to per-frame signaling.
    uint64_t                                 m_maxSubmittedInputReleaseId;
    uint64_t                                 m_lastSignaledInputReleaseId;
    VkVideoCodingControlFlagsKHR             m_controlCmd;
    VkSharedBaseObj<VulkanVideoImagePool>    m_linearInputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_inputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_dpbImagePool;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
    VkSharedBaseObj<VulkanVideoImagePool>    m_inputSubsampledImagePool;
#endif // NV_AQ_GPU_LIB_SUPPORTED
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    VkSharedBaseObj<VulkanFilter>            m_inputComputeFilter;
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // Filter-dispatch observable; see the accessors at the top of the class.
    // Declared OUTSIDE the compute-filter ifdef so the member layout of this
    // class does not depend on that macro -- the counters simply stay 0 in a
    // build with no filter, which is the honest answer there.
    //
    // Atomic because the recording site runs on whatever thread drives
    // EncodeFrame while GetCompletionInfo() is documented threading class
    // (c) -- callable concurrently. Relaxed ordering: these are counters
    // read for reporting, and they order nothing.
    std::atomic<uint32_t>                    m_inputFilterKind{0};
    std::atomic<uint64_t>                    m_inputFilterDispatchCount{0};
    std::atomic<uint64_t>                    m_stagedCopyCount{0};
    // Atomic and relaxed for the same reason the three counters above are:
    // written on whatever thread drives EncodeFrame, read by
    // GetCompletionInfo() which is documented threading class (c).
    std::atomic<uint64_t>                    m_foreignAcquireCount{0};
    std::atomic<uint64_t>                    m_localAcquireCount{0};
    // See GetContentProbe(). NOT owned here: the ext layer creates it and
    // injects it, because arming and reporting must work on a null-backend
    // session that has no encoder object at all. This class supplies the one
    // thing the ext layer cannot -- a command buffer in which the imported
    // image is readable -- and nothing else.
    VkSharedBaseObj<VkVideoEncoderContentProbe> m_contentProbe;
    // Captured in InitEncoder so SetContentProbe() can Configure whenever it
    // is called. Zero until InitEncoder has run.
    uint32_t                                 m_contentProbeQueueDepth = 0;
    void ConfigureContentProbe();
    VkSharedBaseObj<VulkanCommandBufferPool> m_inputCommandBufferPool;

    // The ONE queue the staged-input batch runs on, and the queue FAMILY that
    // queue belongs to. Read by StageInputFrame (for the FOREIGN -> local
    // acquire's destination family, on BOTH branches) and by
    // SubmitStagedInputFrame (for the queue). Two reads of one fact, because
    // the two must never be able to disagree: a queue-family acquire executes
    // on a queue of its DESTINATION family or it is invalid, and the failure
    // presents as a wedged queue -- a hang, which a timing-out test reads as
    // flakiness rather than as a defect.
    //
    // The queue is a SESSION property, not a per-frame one, and it has to be:
    // both branches take their command buffer from m_inputCommandBufferPool,
    // which InitEncoder creates on ONE family -- the compute family when the
    // preprocess filter exists (the filter IS that pool), the transfer or
    // encode family otherwise -- and a command buffer may only be submitted
    // to a queue of the family its pool was created for
    // (VUID-vkQueueSubmit2-commandBuffer-03874). So the branch that ran
    // cannot select the queue independently; what it must not do is name a
    // different family in its barriers than the one this submit uses, which
    // is precisely what these two accessors prevent.
    VulkanDeviceContext::QueueFamilySubmitType GetStagedInputSubmitType() const;
    uint32_t GetStagedInputQueueFamilyIdx() const;

    VkSharedBaseObj<VulkanCommandBufferPool> m_encodeCommandBufferPool;
    VulkanBitstreamBufferPool                m_bitstreamBuffersQueue;
#ifdef VIDEO_DISPLAY_QUEUE_SUPPORT
    DisplayQueue                             m_displayQueue;
#endif // VIDEO_DISPLAY_QUEUE_SUPPORT
    EncoderFrameQueue                        m_encoderThreadQueue;
    std::thread                              m_encoderQueueConsumerThread;
    VkSharedBaseObj<VkVideoEncodeFrameInfo>  m_lastDeferredFrame;
    VkSemaphore                              m_hwLoadBalancingTimelineSemaphore;
    int32_t                                  m_currentVideoQueueIndx;
    // Completion timeline (ext API currency 3): created by the ext layer at
    // init, before any submit -- which is what lets the submit path read
    // the handle without a lock; GPU-signaled at queue flush points with
    // max(externalFrameId)+1. GPU ordering only -- see the ext header.
    VkSemaphore                              m_completionTimelineSemaphore;
    bool                                     m_completionSemaphoreExportable;
    uint64_t                                 m_maxSubmittedCompletionValue;
    uint64_t                                 m_lastSignaledCompletionValue;

    VkFormat                                 m_imageQpMapFormat;
    VkExtent2D                               m_qpMapTexelSize;
    VkImageTiling                            m_qpMapTiling;
    VkSharedBaseObj<VulkanVideoImagePool>    m_linearQpMapImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_qpMapImagePool;
    VkSharedBaseObj<VkVideoEncoderPsnr>       m_psnr;

    VkVideoCrc                               m_crc;

#ifdef NV_AQ_GPU_LIB_SUPPORTED
    std::shared_ptr<nvenc_aq::EncodeAqAnalyzes > m_aqAnalyzes;
#endif // NV_AQ_GPU_LIB_SUPPORTED

    // THE single site that brings the assembly workers up. InitEncoder used
    // to inline this; it is a function because a non-terminal drain has to
    // run it a second time, and two copies of "how the assembly pipeline is
    // started" is how the two drift apart.
    bool StartAssemblyThreads();

    bool                                     m_asyncAssemblyEnabled{false};
    uint32_t                                 m_assemblyQueueCapacity{0};
    AssemblyQueue                            m_assemblyQueue;
    std::vector<std::thread>                 m_assemblyThreads;

    // In-memory completion-record queue. On the ASSEMBLY-WORKER path every
    // completed frame publishes a record here through PushCapturedBitstream
    // when a drain-capable consumer exists (capture mode, or a registered
    // completion subscriber); the encoded bytes are carried only in capture
    // mode (disableFileOutput) -- in file-output mode the payload went to the
    // file and the record is metadata plus the per-frame result. Drained
    // by VulkanVideoEncoderExtImpl via TryPopCapturedBitstream().
    //
    // The SYNCHRONOUS assembly path is deliberately OUTSIDE that sentence and
    // publishes nothing: AssembleBitstreamData writes through the non-
    // publishing WriteBitstreamToFileOutput and reaches no PushCapturedBitstream
    // at all. That is precisely why ProcessOrderedFrames and
    // ProcessOutOfOrderFrames both hard-refuse to run it while a completion
    // subscriber is registered -- otherwise frames would be encoded correctly
    // and reported to nobody.
public:
    // Single completion edge (M6): raised from PushCapturedBitstream and
    // nowhere else, after every frame completion on the ASSEMBLY-WORKER path,
    // in both output modes. Set once by the Ext layer. The synchronous
    // assembly path raises no edge either -- see the queue comment above; the
    // encoder-sync-assembly test asserts exactly that, in both output modes.
    //
    // THREADING, and read this before writing a callback: the callback runs
    // with m_capturedBitstreamsMutex RELEASED (so it may re-enter the
    // thread-safe retrieval methods) but with the ASSEMBLY ORDERING LOCK
    // (m_assemblyFileMutex) STILL HELD, because both capture paths publish
    // from inside their ordering turn. The callback therefore MUST NOT BLOCK
    // and MUST NOT wait on anything an assembly worker could produce -- doing
    // so stalls every worker and wedges the pipeline. Chromium's callback is a
    // post-to-sequence trampoline, which satisfies this by construction.
    void SetOnBitstreamCaptured(std::function<void(uint64_t)> callback) {
        std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
        m_onBitstreamCaptured = std::move(callback);
    }

    void NotifyBitstreamCaptured(uint64_t frameId) {
        std::function<void(uint64_t)> callback;
        {
            std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
            callback = m_onBitstreamCaptured;
        }
        if (callback) {
            callback(frameId);
        }
    }

protected:
    struct CapturedBitstream {
        uint64_t frameId;
        std::vector<uint8_t> bytes;
        bool isIdr;
        uint32_t pictureType;
        // Per-frame result. VK_SUCCESS for a normal capture; the
        // readback/assembly failure code (with empty bytes) otherwise, so
        // the Ext caller gets an actionable per-frame error instead of a
        // frame that silently never becomes ready.
        VkResult status = VK_SUCCESS;
    };
    std::function<void(uint64_t)>            m_onBitstreamCaptured;
    mutable std::mutex                               m_capturedBitstreamsMutex;
    std::deque<CapturedBitstream>            m_capturedBitstreams;

    // THE single point where a frame's completion becomes visible to the
    // consumer. Every codec path must publish through here, in BOTH output
    // modes -- the completion edge does not depend on disableFileOutput.
    // What is STORED depends on who can drain it:
    //   - capture mode (disableFileOutput): the record carries the encoded
    //     bytes; the FIFO is the payload channel.
    //   - file-output mode with a completion subscriber (the Ext layer
    //     registers one at InitializeExt): the record carries metadata only
    //     (empty bytes -- the payload went to the file); the subscriber
    //     drains it on this very edge, so the FIFO never accumulates.
    //   - file-output mode with no subscriber (the file-based CLI apps):
    //     nothing is stored, because nothing would ever drain it, and the
    //     notify below degenerates to a no-op.
    // The insert and the edge-raise stay paired in one place precisely so
    // that a path added later cannot complete a frame and silently omit the
    // edge. The captured-bitstreams lock is dropped before notifying, so the
    // callback may re-enter the retrieval methods; both capture paths hold
    // the assembly ordering lock across this call, so the callback runs
    // under it (see SetOnBitstreamCaptured -- it must not block).
    void PushCapturedBitstream(CapturedBitstream&& cap) {
        const uint64_t frameId = cap.frameId;
        {
            std::lock_guard<std::mutex> lock(m_capturedBitstreamsMutex);
            const bool captureMode =
                (m_encoderConfig && (m_encoderConfig->disableFileOutput != 0));
            if (captureMode || (m_onBitstreamCaptured != nullptr)) {
                m_capturedBitstreams.push_back(std::move(cap));
            }
        }
        NotifyBitstreamCaptured(frameId);
    }
    std::atomic<uint64_t>                    m_assemblySequenceCounter{0};
    std::atomic<uint64_t>                    m_nextWriteSequence{0};
    std::mutex                               m_assemblyFileMutex;
    std::condition_variable                  m_assemblyOrderCV;
    std::atomic<uint32_t>                    m_assemblyErrorCount{0};
    // Frames that could not be processed. The assembly counter above speaks
    // only for the async-assembly workers; a failure raised while the frame
    // was being recorded or submitted happens in PushOrderedFrames, which
    // counts it there. That is what lets the drain speak for the frames
    // already pushed and released during the run, and not only for the last
    // one. Both counters are monotonic for the life of the session and are
    // zeroed with the rest of the per-session counters.
    std::atomic<uint32_t>                    m_frameProcessingErrorCount{0};
};

VkResult CreateVideoEncoderH264(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder);

VkResult CreateVideoEncoderH265(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder);

VkResult CreateVideoEncoderAV1(const VulkanDeviceContext* vkDevCtx,
                               VkSharedBaseObj<EncoderConfig>& encoderConfig,
                               VkSharedBaseObj<VkVideoEncoder>& encoder);

#endif /* _VKVIDEOENCODER_VKVIDEOENCODER_H_ */
