/*
* Copyright 2020 NVIDIA Corporation.
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

#ifndef LIBS_VKCODECUTILS_VULKANDECODERFRAMEPROCESSOR_H_
#define LIBS_VKCODECUTILS_VULKANDECODERFRAMEPROCESSOR_H_

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VkVideoQueue.h"
#include "VkCodecUtils/VulkanDecodedFrame.h"

class FrameProcessor;
class VulkanDeviceContext;
template<class FrameDataType> class VulkanFrame;

class DecoderFrameProcessorState
{
public:
    VkResult Init(const VulkanDeviceContext* vkDevCtx,
                  VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>>& videoQueue,
                  int32_t maxNumberOfFrames = 0);

    DecoderFrameProcessorState()
    : m_vulkanFrame()
    , m_frameProcessor()
    , m_maxNumberOfFrames(0) {}

    DecoderFrameProcessorState(const VulkanDeviceContext* vkDevCtx,
                               VkSharedBaseObj<VkVideoQueue<VulkanDecodedFrame>>& videoQueue,
                               int32_t maxNumberOfFrames = 0)
    : m_vulkanFrame()
    , m_frameProcessor()
    , m_maxNumberOfFrames(0)
    {
        VkResult result = Init(vkDevCtx, videoQueue, maxNumberOfFrames);
        if (result != VK_SUCCESS) {
            assert(!"DecoderFrameProcessorState::Init() has failed");
        }
    }

    void Deinit();

    ~DecoderFrameProcessorState()
    {
        Deinit();
    }

    VulkanFrame<VulkanDecodedFrame>* GetVulkanFrame() { return m_vulkanFrame.get(); }

    operator VkSharedBaseObj<FrameProcessor>&()
    {
        return m_frameProcessor;
    }

    operator const VkSharedBaseObj<FrameProcessor>&() const
    {
        return m_frameProcessor;
    }

    VkSharedBaseObj<FrameProcessor>& operator->() {
        return m_frameProcessor;
    }

    const VkSharedBaseObj<FrameProcessor>& operator->() const {
        return m_frameProcessor;
    }

private:
    VkSharedBaseObj<VulkanFrame<VulkanDecodedFrame>> m_vulkanFrame;
    VkSharedBaseObj<FrameProcessor>                  m_frameProcessor;
    int32_t                                          m_maxNumberOfFrames;
};

#endif /* LIBS_VKCODECUTILS_VULKANDECODERFRAMEPROCESSOR_H_ */
