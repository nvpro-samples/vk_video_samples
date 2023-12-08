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
#include "VkEncoderDpbH264.h"
#include "VkCodecUtils/VulkanVideoEncodeDisplayQueue.h"
#include "VkShell/Shell.h"
#include "mio/mio.hpp"

class VkVideoEncoderH264;
class VkVideoEncoderH265;

class VkVideoEncoder : public VkVideoRefCountBase {

public:

    using VulkanBitstreamBufferPool = VulkanVideoRefCountedPool<VulkanBitstreamBufferImpl, 64>;

    enum { MAX_IMAGE_REF_RESOURCES = 17 }; /* List of reference pictures 16 + 1 for current */
    enum { MAX_BITSTREAM_HEADER_BUFFER_SIZE = 256 };

    struct VkVideoEncodeFrameInfo : public VkVideoRefCountBase
    {
        VkStructureType GetType() {
            return (encodeInfo.pNext == nullptr) ?
                    VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR : ((VkBaseInStructure*)encodeInfo.pNext)->sType;
        }

        VkVideoEncodeFrameInfo(const void* pNext = nullptr)
            : encodeInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR, pNext}
            , frameInputOrderNum(uint64_t(-1))
            , frameEncodeOrderNum(uint64_t(-1))
            , positionInGopInDisplayOrder(uint8_t(-1))
            , positionInGopInDecodeOrder(uint8_t(-1))
            , picOrderCntVal(-1)
            , pictureType(VkVideoGopStructure::FRAME_TYPE_IDR)
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
            , rateControlLayersInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR }
            , referenceSlotsInfo{}
            , setupReferenceSlotInfo{ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR }
            , videoSession()
            , videoSessionParameters()
            , srcStagingImageView()
            , srcEncodeImageResource()
            , setupImageResource()
            , outputBitstreamBuffer()
            , dpbImageResources()
            , m_refCount(0)
            , m_parent()
            , m_parentIndex(-1)
        {
            assert(ARRAYSIZE(referenceSlotsInfo) == MAX_IMAGE_REF_RESOURCES);
            for (uint32_t i = 0; i < MAX_IMAGE_REF_RESOURCES; i++) {
                referenceSlotsInfo[i].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            }
            assert(numDpbImageResources <= ARRAYSIZE(dpbImageResources));
            for (uint32_t i = 0; i < numDpbImageResources; i++) {
               dpbImageResources[i] = nullptr;
            }
            numDpbImageResources = 0;
        }

        VkVideoEncodeInfoKHR                               encodeInfo;
        uint64_t                                           frameInputOrderNum;
        uint64_t                                           frameEncodeOrderNum;         // == display order
        uint8_t                                            positionInGopInDisplayOrder; // == display order
        uint8_t                                            positionInGopInDecodeOrder;  // == decode order
        int32_t                                            picOrderCntVal;
        VkVideoGopStructure::FrameType                     pictureType;
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
        VkVideoReferenceSlotInfoKHR                        setupReferenceSlotInfo;
        VkSharedBaseObj<VulkanVideoSession>                videoSession;
        VkSharedBaseObj<VulkanVideoSessionParameters>      videoSessionParameters;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcStagingImageView;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          srcEncodeImageResource;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          setupImageResource;
        VkSharedBaseObj<VulkanBitstreamBuffer>             outputBitstreamBuffer;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          dpbImageResources[MAX_IMAGE_REF_RESOURCES];
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> inputCmdBuffer;
        VkSharedBaseObj<VulkanCommandBufferPool::PoolNode> encodeCmdBuffer;
        VkSharedBaseObj<VkVideoEncodeFrameInfo>            dependantFrames;

        VkResult SyncHostOnCmdBuffComplete() {

            if (inputCmdBuffer) {
                inputCmdBuffer->ResetCommandBuffer(true);
            }

            if (encodeCmdBuffer) {
                encodeCmdBuffer->ResetCommandBuffer(true);
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

        virtual void Reset(bool releaseResources = true) {
            // Clear and check state
            assert(encodeInfo.sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR);
            assert(encodeInfo.pNext != nullptr);

            if ((frameInputOrderNum == (uint64_t)-1) &&
                (frameEncodeOrderNum = (uint64_t)-1)) {
                // it is already reset
                return;
            }

            frameInputOrderNum = (uint64_t)-1;          // For debugging
            frameEncodeOrderNum = (uint64_t)-1;         // For debugging
            positionInGopInDisplayOrder  = uint8_t(-1); // For debugging
            positionInGopInDecodeOrder = uint8_t(-1);   // For debugging
            picOrderCntVal = -1; // For debugging
            pictureType = VkVideoGopStructure::FRAME_TYPE_INVALID;
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
                outputBitstreamBuffer = nullptr;
                assert(numDpbImageResources <= ARRAYSIZE(dpbImageResources));
                for (uint32_t i = 0; i < numDpbImageResources; i++) {
                   dpbImageResources[i] = nullptr;
                }
                numDpbImageResources = 0;
                inputCmdBuffer = nullptr;
                encodeCmdBuffer = nullptr;

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

            Reset();

            return VK_SUCCESS;
        }

    private:
        std::atomic<int32_t>                m_refCount;
        VkSharedBaseObj<VulkanBufferPoolIf> m_parent;
        int32_t                             m_parentIndex;
    };

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
                    m_runLoopThread.join();
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

public:
    VkVideoEncoder(const VulkanDeviceContext* vkDevCtx)
        : refCount(0)
        , m_encoderConfig()
        , m_vkDevCtx(vkDevCtx)
        , m_inputFrameNum(0)
        , m_encodeFrameNum(0)
        , m_videoSession()
        , m_videoSessionParameters()
        , m_imageDpbFormat()
        , m_imageInFormat()
        , m_maxCodedExtent()
        , m_maxActiveReferencePictures(16)
        , m_minStreamBufferSize(2 * 1024 * 1024)
        , m_streamBufferSize(m_minStreamBufferSize)
        , m_rateControlInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR }
        , m_rateControlLayersInfo{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR }
        , m_picIdxToDpb{}
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
        , m_enableEncoderQueue(false)
        , m_verbose(false)
        , m_numDeferredFrames()
        , m_controlCmd(VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR |
                       VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR |
                       VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR)
        , m_linearInputImagePool()
        , m_inputImagePool()
        , m_dpbImagePool()
        , m_inputCommandBufferPool()
        , m_encodeCommandBufferPool()
        , m_bitstreamBuffersQueue()
        , m_displayQueue()
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

    VkResult AttachDisplayQueue(VkSharedBaseObj<Shell>& displayShell,
                                VkSharedBaseObj<VulkanVideoDisplayQueue<VulkanEncoderInputFrame>>& videoDispayQueue)
    {
        return m_displayQueue.AttachDisplayQueue(displayShell, videoDispayQueue);
    }

    virtual VkResult CreateFrameInfoBuffersQueue(uint32_t numPoolNodes) = 0;
    virtual bool GetAvailablePoolNode(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) = 0;

    virtual VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>& encoderConfig) = 0; // Must be implemented by the codec
    VkResult LoadNextFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult StageInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    VkResult SubmitStagedInputFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);
    virtual VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo) = 0; // Must be implemented by the codec
    virtual VkResult HandleCtrlCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo);


    VkResult RecordVideoCodingCmd(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                  uint32_t frameIdx, uint32_t ofTotalFrames);

    VkResult RecordVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, uint32_t numFrames);

    VkResult SubmitVideoCodingCmds(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                   uint32_t frameIdx, uint32_t ofTotalFrames);

    VkResult AssembleBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                   uint32_t frameIdx, uint32_t ofTotalFrames);

    VkResult PrintVideoCodingLink(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, uint32_t frameIdx, uint32_t ofTotalFrames)
    {
        if (m_encoderConfig->verbose) {
            std::cout << "Frame: " << frameIdx << " of " << ofTotalFrames
                      << " " << VkVideoGopStructure::GetFrameTypeName(encodeFrameInfo->pictureType)
                      << ", frameEncodeOrderNum: " << (uint32_t)encodeFrameInfo->frameEncodeOrderNum
                      << ", GOP display order: " << (uint32_t)encodeFrameInfo->positionInGopInDisplayOrder
                      << ", GOP decode  order: " << (uint32_t)encodeFrameInfo->positionInGopInDecodeOrder << std::endl << std::flush;
        }
        return VK_SUCCESS;
    }

    virtual VkResult InitRateControl(VkCommandBuffer cmdBuf, uint32_t qp) = 0; // Must be implemented by the codec

    const uint8_t* setPlaneOffset(const uint8_t* pFrameData, size_t bufferSize, size_t &currentReadOffset);

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
                                      uint32_t srcCopyArrayLayer = 0,
                                      uint32_t dstCopyArrayLayer = 0,
                                      VkImageLayout srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VkImageLayout dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    virtual VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo,
                                uint32_t frameIdx, uint32_t ofTotalFrames) = 0;

    virtual ~VkVideoEncoder() {
        DeinitEncoder();
    }

    int32_t DeinitEncoder();

    bool EnqueueFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>& encodeFrameInfo, bool preFlushQueue, bool postFlushQueue) {

        if (preFlushQueue) {
            PushOrderedFrames();
        }
        InsertOrdered(encodeFrameInfo);
        if (postFlushQueue) {
            PushOrderedFrames();
        }
        return true;
    }

    void ConsumerThread();

    void InsertOrdered(VkSharedBaseObj<VkVideoEncodeFrameInfo>& current,
                       VkSharedBaseObj<VkVideoEncodeFrameInfo>& prev,
                       VkSharedBaseObj<VkVideoEncodeFrameInfo>& node) {

        if ((current == nullptr) || (current->positionInGopInDecodeOrder >= node->positionInGopInDecodeOrder)) {

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
    void InsertOrdered(VkSharedBaseObj<VkVideoEncodeFrameInfo>& dependantFrames) {
        if (m_lastDeferredFrame == nullptr) {
            m_lastDeferredFrame = dependantFrames;
            m_numDeferredFrames++;
            return;
        }
        VkSharedBaseObj<VkVideoEncodeFrameInfo> prev;
        InsertOrdered(m_lastDeferredFrame, prev, dependantFrames);
        m_numDeferredFrames++;
    }

    VkResult PushOrderedFrames();
    VkResult ProcessOrderedFrames(VkSharedBaseObj<VkVideoEncodeFrameInfo>& frames, uint32_t numFrames);

    typedef VkThreadSafeQueue<VkSharedBaseObj<VkVideoEncodeFrameInfo>> EncoderFrameQueue;

private:
    std::atomic<int32_t> refCount;
protected:
    VkSharedBaseObj<EncoderConfig>                m_encoderConfig;
    const VulkanDeviceContext*                    m_vkDevCtx;
    uint64_t                                      m_inputFrameNum;
    uint64_t                                      m_encodeFrameNum;
    VkSharedBaseObj<VulkanVideoSession>           m_videoSession;
    VkSharedBaseObj<VulkanVideoSessionParameters> m_videoSessionParameters;
    VkFormat                              m_imageDpbFormat;
    VkFormat                              m_imageInFormat;
    VkExtent2D                            m_maxCodedExtent;
    uint32_t                              m_maxActiveReferencePictures;
    size_t                                m_minStreamBufferSize;
    size_t                                m_streamBufferSize;
    VkVideoEncodeQualityLevelInfoKHR      m_qualityLevelInfo;
    VkVideoEncodeRateControlInfoKHR       m_rateControlInfo;
    VkVideoEncodeRateControlLayerInfoKHR  m_rateControlLayersInfo[1];
    int8_t   m_picIdxToDpb[17]; // MAX_DPB_SLOTS + 1
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
    uint32_t m_enableEncoderQueue : 1;
    uint32_t m_verbose : 1;
    uint32_t                                 m_numDeferredFrames;
    VkVideoCodingControlFlagsKHR             m_controlCmd;
    VkSharedBaseObj<VulkanVideoImagePool>    m_linearInputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_inputImagePool;
    VkSharedBaseObj<VulkanVideoImagePool>    m_dpbImagePool;
    VkSharedBaseObj<VulkanCommandBufferPool> m_inputCommandBufferPool;
    VkSharedBaseObj<VulkanCommandBufferPool> m_encodeCommandBufferPool;
    VulkanBitstreamBufferPool                m_bitstreamBuffersQueue;
    DisplayQueue                             m_displayQueue;
    EncoderFrameQueue                        m_encoderQueue;
    std::thread                              m_encoderQueueConsumerThread;
    VkSharedBaseObj<VkVideoEncodeFrameInfo>  m_lastDeferredFrame;
};

VkResult CreateVideoEncoderH264(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder);

VkResult CreateVideoEncoderH265(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<EncoderConfig>& encoderConfig,
                                VkSharedBaseObj<VkVideoEncoder>& encoder);

#endif /* _VKVIDEOENCODER_VKVIDEOENCODER_H_ */
