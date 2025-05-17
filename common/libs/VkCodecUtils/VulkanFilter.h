/*
 * Copyright 2023 NVIDIA Corporation.
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

#ifndef _VKCODECUTILS_VULKANFILTER_H_

#include <atomic>
#include <string>

#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VulkanShaderCompiler.h"
#include "VkCodecUtils/VkImageResource.h"
#include "VkCodecUtils/VulkanCommandBufferPool.h"
#include "VkCodecUtils/VulkanSemaphoreDump.h"

struct VulkanShaderInput {
    const std::string     shader;
    VkShaderStageFlagBits type;
    uint32_t              shaderIsFsPath : 1;
};

class VulkanFilter : public VulkanCommandBufferPool
{
public:
    // Constants moved inside the class as static constexpr
    static constexpr uint32_t MAX_SEMAPHORES = 4;
    static constexpr uint32_t MAX_CMD_BUFFERS = 4;

    VulkanFilter(const VulkanDeviceContext* vkDevCtx,
                 uint32_t queueFamilyIndex,
                 uint32_t queueIndex)
        : VulkanCommandBufferPool(vkDevCtx),
          m_vulkanShaderCompiler(),
          m_queueFamilyIndex(queueFamilyIndex),
          m_queueIndex(queueIndex),
          m_queue()
    {
        m_vkDevCtx->GetDeviceQueue(*m_vkDevCtx, queueFamilyIndex, queueIndex, &m_queue);
        assert(m_queue != VK_NULL_HANDLE);
    }

    virtual ~VulkanFilter()
    {
        assert(m_vkDevCtx != nullptr);
    }

    VkShaderModule CreateShaderModule(const char *shaderCode, size_t shaderSize,
                                      VkShaderStageFlagBits type)
    {
        return m_vulkanShaderCompiler.BuildGlslShader(shaderCode, shaderSize,
                                                      type,
                                                      m_vkDevCtx);
    }

    void DestroyShaderModule(VkShaderModule shaderModule)
    {
        if (shaderModule != VK_NULL_HANDLE) {
            m_vkDevCtx->DestroyShaderModule(*m_vkDevCtx, shaderModule, nullptr);
        }
    }

    virtual VkResult RecordCommandBuffer(VkCommandBuffer cmdBuf,
                                         const VkImageResourceView* inputImageView,
                                         const VkVideoPictureResourceInfoKHR * inputImageResourceInfo,
                                         const VkImageResourceView* outputImageView,
                                         const VkVideoPictureResourceInfoKHR * outputImageResourceInfo,
                                         uint32_t bufferIdx) = 0;

    virtual VkResult SubmitCommandBuffer(uint32_t commandBufferCount,
                                         const VkCommandBuffer* pCommandBuffers,
                                         uint32_t waitSemaphoreCount,
                                         const VkSemaphore* pWaitSemaphores,
                                         const VkPipelineStageFlags2KHR* pWaitStageMasks,
                                         uint32_t signalSemaphoreCount,
                                         const VkSemaphore* pSignalSemaphores,
                                         const VkPipelineStageFlags2KHR* pSignalStageMasks,
                                         VkFence filterCompleteFence) const
    {
        assert(m_queue != VK_NULL_HANDLE);
        assert(commandBufferCount <= MAX_CMD_BUFFERS);
        assert(waitSemaphoreCount <= MAX_SEMAPHORES);
        assert(signalSemaphoreCount <= MAX_SEMAPHORES);

        // Prepare command buffer info on stack
        VkCommandBufferSubmitInfoKHR cmdBufferInfos[MAX_CMD_BUFFERS];
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            cmdBufferInfos[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR;
            cmdBufferInfos[i].pNext = nullptr;
            cmdBufferInfos[i].commandBuffer = pCommandBuffers[i];
            cmdBufferInfos[i].deviceMask = 0;
        }

        // Prepare wait semaphore info on stack
        VkSemaphoreSubmitInfoKHR waitSemaphoreInfos[MAX_SEMAPHORES];
        for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
            waitSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            waitSemaphoreInfos[i].pNext = nullptr;
            waitSemaphoreInfos[i].semaphore = pWaitSemaphores[i];
            waitSemaphoreInfos[i].value = 0; // Binary semaphore
            waitSemaphoreInfos[i].stageMask = pWaitStageMasks[i];
            waitSemaphoreInfos[i].deviceIndex = 0;
        }

        // Prepare signal semaphore info on stack
        VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[MAX_SEMAPHORES];
        for (uint32_t i = 0; i < signalSemaphoreCount; i++) {
            signalSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            signalSemaphoreInfos[i].pNext = nullptr;
            signalSemaphoreInfos[i].semaphore = pSignalSemaphores[i];
            signalSemaphoreInfos[i].value = 0; // Binary semaphore
            signalSemaphoreInfos[i].stageMask = pSignalStageMasks[i];
            signalSemaphoreInfos[i].deviceIndex = 0;
        }

        // Submit info
        VkSubmitInfo2KHR submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR;
        submitInfo.pNext = nullptr;
        submitInfo.flags = 0;
        submitInfo.waitSemaphoreInfoCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphoreInfos = waitSemaphoreInfos;
        submitInfo.commandBufferInfoCount = commandBufferCount;
        submitInfo.pCommandBufferInfos = cmdBufferInfos;
        submitInfo.signalSemaphoreInfoCount = signalSemaphoreCount;
        submitInfo.pSignalSemaphoreInfos = signalSemaphoreInfos;

        if (false) {
            // Dump semaphore info for debugging
            VulkanSemaphoreDump::DumpSemaphoreInfo(submitInfo, "DECODE FILTER", 0);
        }

        assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, filterCompleteFence));
        VkResult result = m_vkDevCtx->QueueSubmit2KHR(m_queue, 1, &submitInfo, filterCompleteFence);

        return result;
    }

    virtual VkResult SubmitCommandBuffer(uint32_t commandBufferCount,
                                         const VkCommandBuffer* pCommandBuffers,
                                         uint32_t waitSemaphoreCount,
                                         const VkSemaphore* pWaitSemaphores,
                                         const uint64_t* pWaitSemaphoreValues,
                                         const VkPipelineStageFlags2KHR* pWaitStageMasks,
                                         uint32_t signalSemaphoreCount,
                                         const VkSemaphore* pSignalSemaphores,
                                         const uint64_t* pSignalSemaphoreValues,
                                         const VkPipelineStageFlags2KHR* pSignalStageMasks,
                                         VkFence filterCompleteFence) const
    {
        assert(m_queue != VK_NULL_HANDLE);
        assert(commandBufferCount <= MAX_CMD_BUFFERS);
        assert(waitSemaphoreCount <= MAX_SEMAPHORES);
        assert(signalSemaphoreCount <= MAX_SEMAPHORES);

        // Prepare command buffer info on stack
        VkCommandBufferSubmitInfoKHR cmdBufferInfos[MAX_CMD_BUFFERS];
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            cmdBufferInfos[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR;
            cmdBufferInfos[i].pNext = nullptr;
            cmdBufferInfos[i].commandBuffer = pCommandBuffers[i];
            cmdBufferInfos[i].deviceMask = 0;
        }

        // Prepare wait semaphore info on stack
        VkSemaphoreSubmitInfoKHR waitSemaphoreInfos[MAX_SEMAPHORES];
        for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
            waitSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            waitSemaphoreInfos[i].pNext = nullptr;
            waitSemaphoreInfos[i].semaphore = pWaitSemaphores[i];
            waitSemaphoreInfos[i].value = pWaitSemaphoreValues[i]; // Timeline value
            waitSemaphoreInfos[i].stageMask = pWaitStageMasks[i];
            waitSemaphoreInfos[i].deviceIndex = 0;
        }

        // Prepare signal semaphore info on stack
        VkSemaphoreSubmitInfoKHR signalSemaphoreInfos[MAX_SEMAPHORES];
        for (uint32_t i = 0; i < signalSemaphoreCount; i++) {
            signalSemaphoreInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            signalSemaphoreInfos[i].pNext = nullptr;
            signalSemaphoreInfos[i].semaphore = pSignalSemaphores[i];
            signalSemaphoreInfos[i].value = pSignalSemaphoreValues[i]; // Timeline value
            signalSemaphoreInfos[i].stageMask = pSignalStageMasks[i];
            signalSemaphoreInfos[i].deviceIndex = 0;
        }

        // Submit info
        VkSubmitInfo2KHR submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR;
        submitInfo.pNext = nullptr;
        submitInfo.flags = 0;
        submitInfo.waitSemaphoreInfoCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphoreInfos = waitSemaphoreInfos;
        submitInfo.commandBufferInfoCount = commandBufferCount;
        submitInfo.pCommandBufferInfos = cmdBufferInfos;
        submitInfo.signalSemaphoreInfoCount = signalSemaphoreCount;
        submitInfo.pSignalSemaphoreInfos = signalSemaphoreInfos;

        if (false) {
            // Dump semaphore info for debugging
            VulkanSemaphoreDump::DumpSemaphoreInfo(submitInfo, "DECODE FILTER", 0);
        }

        assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, filterCompleteFence));
        VkResult result = m_vkDevCtx->QueueSubmit2KHR(m_queue, 1, &submitInfo, filterCompleteFence);

        return result;
    }


protected:
    VulkanShaderCompiler m_vulkanShaderCompiler;
    uint32_t             m_queueFamilyIndex;
    uint32_t             m_queueIndex;
    VkQueue              m_queue;
};
#endif /* _VKCODECUTILS_VULKANFILTER_H_ */
