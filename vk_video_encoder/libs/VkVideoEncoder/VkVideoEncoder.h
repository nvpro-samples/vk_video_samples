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
#include <thread>
#include <atomic>
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

class VkVideoEncoderH264;
class VkVideoEncoderH265;
class VkVideoEncoderAV1;

class VkVideoEncoder : public VkVideoRefCountBase {

public:

    using VulkanBitstreamBufferPool = VulkanVideoRefCountedPool<VulkanBitstreamBufferImpl, 64>;

    enum { MAX_IMAGE_REF_RESOURCES = 17 }; /* List of reference pictures 16 + 1 for current */
    enum { MAX_BITSTREAM_HEADER_BUFFER_SIZE = 256 };

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
            , m_refCount(0)
            , m_parent()
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

        virtual int32_t AddRef()
        {
            return ++m_refCount;
        }

        virtual int32_t Release()
        {
            uint32_t ret = --m_refCount;
            if (ret == 1) {
                m_parent->ReleasePoolNodeToPool(m_parentIndex);
                m_parentIndex = -1;
                m_parent = nullptr;
                Reset();
            } else if (ret == 0) {
                // Destroy the resources if ref-count reaches zero
            }
            return ret;
        }

        void Init() {
            AddRef();
            Reset();
        }

        void Deinit() {
            Reset();
            Release();
        }

        VkResult SetParent(VulkanBufferPoolIf* buffPool, int32_t parentIndex)
        {
            assert(m_parent == nullptr);
            m_parent      = buffPool;
            assert(m_parentIndex == -1);
            m_parentIndex = parentIndex;

            return VK_SUCCESS;
        }

    private:
        std::atomic<int32_t>                m_refCount;
        VkSharedBaseObj<VulkanBufferPoolIf> m_parent;
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
        : refCount(0)
        , m_encoderConfig()
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
        , m_imageQpMapFormat()
        , m_qpMapTexelSize()
        , m_qpMapTiling()
        , m_linearQpMapImagePool()
        , m_qpMapImagePool()
    { }

    // Factory Function
    static VkResult CreateVideoEncoder(const VulkanDeviceContext* vkDevCtx,
                                       VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                       VkSharedBaseObj<VkVideoEncoder>& encoder);

    virtual int32_t AddRef()
    {
        return ++refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --refCount;
        // Destroy the device if ref-count reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

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
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        const uint64_t* pWaitSemaphoreValues,
        const VkPipelineStageFlags2* pWaitDstStageMasks,
        uint32_t signalSemaphoreCount,
        const VkSemaphore* pSignalSemaphores,
        const uint64_t* pSignalSemaphoreValues);

    // Helper: wrap an external VkImage as a VulkanVideoImagePoolNode
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

    virtual VkResult SubmitVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                           uint32_t frameIdx, uint32_t ofTotalFrames);

    virtual VkResult AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                           uint32_t frameIdx, uint32_t ofTotalFrames);

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

    bool WaitForThreadsToComplete();

protected:

    // Called by the InitEncoderCodec to initialize the common encoder code.
    VkResult InitEncoder(VkSharedBaseObj<EncoderConfig>& encoderConfig);

    VkDeviceSize GetBitstreamBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer);

    VkImageLayout TransitionImageLayout(VkCommandBuffer cmdBuf,
                                        VkSharedBaseObj<VkImageResourceView>& imageView,
                                        VkImageLayout oldLayout, VkImageLayout newLayout);

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

    virtual ~VkVideoEncoder() {
        DeinitEncoder();
    }

    int32_t DeinitEncoder();

    bool EnqueueFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                      bool isIdrFrame, bool isReferenceFrame) {

        const bool preFlushQueue = isIdrFrame;
        if (preFlushQueue) {
            PushOrderedFrames();
        }

        InsertOrdered(encodeFrameInfo, isReferenceFrame);

        const bool postFlushQueue = (encodeFrameInfo->lastFrame ||
                                        (isReferenceFrame && (m_numDeferredRefFrames == m_holdRefFramesInQueue)));
        if (postFlushQueue) {
            PushOrderedFrames();
        }
        return true;
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

    typedef VkThreadSafeQueue<VkSharedBaseObj<VkVideoEncodeFrameInfo>> EncoderFrameQueue;
private:
    std::atomic<int32_t> refCount;
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
    VkVideoCodingControlFlagsKHR             m_controlCmd;
    VkSharedBaseObj<VulkanVideoImagePool>    m_linearInputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_inputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_dpbImagePool;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
    VkSharedBaseObj<VulkanVideoImagePool>    m_inputSubsampledImagePool;
#endif // NV_AQ_GPU_LIB_SUPPORTED
    VkSharedBaseObj<VulkanFilter>            m_inputComputeFilter;
    VkSharedBaseObj<VulkanCommandBufferPool> m_inputCommandBufferPool;
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

    VkFormat                                 m_imageQpMapFormat;
    VkExtent2D                               m_qpMapTexelSize;
    VkImageTiling                            m_qpMapTiling;
    VkSharedBaseObj<VulkanVideoImagePool>    m_linearQpMapImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_qpMapImagePool;
#ifdef NV_AQ_GPU_LIB_SUPPORTED
    std::shared_ptr<nvenc_aq::EncodeAqAnalyzes > m_aqAnalyzes;
#endif // NV_AQ_GPU_LIB_SUPPORTED
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
