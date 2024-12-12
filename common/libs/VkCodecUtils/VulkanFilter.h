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

struct VulkanShaderInput {
    const std::string     shader;
    VkShaderStageFlagBits type;
    uint32_t              shaderIsFsPath : 1;
};

class VulkanFilter : public VulkanCommandBufferPool
{
public:

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
                                         const VkCommandBuffer*  pCommandBuffers,
                                         uint32_t waitSemaphoreCount,
                                         const VkSemaphore* pWaitSemaphores,
                                         uint32_t signalSemaphoreCount,
                                         const VkSemaphore* pSignalSemaphores,
                                         VkFence filterCompleteFence) const
    {

        assert(m_queue != VK_NULL_HANDLE);

        // Wait for rendering finished
        VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        // Submit compute commands
        VkSubmitInfo submitInfo {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pCommandBuffers = pCommandBuffers;
        submitInfo.commandBufferCount = commandBufferCount;
        submitInfo.waitSemaphoreCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphores = pWaitSemaphores;
        submitInfo.pWaitDstStageMask = &waitStageMask;
        submitInfo.signalSemaphoreCount = signalSemaphoreCount;
        submitInfo.pSignalSemaphores = pSignalSemaphores;

        assert(VK_NOT_READY == m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, filterCompleteFence));
        VkResult result = m_vkDevCtx->QueueSubmit(m_queue, 1, &submitInfo, filterCompleteFence);

        if (result != VK_SUCCESS) {
            return result;
        }

        return result;
    }

protected:
    VulkanShaderCompiler m_vulkanShaderCompiler;
    uint32_t             m_queueFamilyIndex;
    uint32_t             m_queueIndex;
    VkQueue              m_queue;
};
#endif /* _VKCODECUTILS_VULKANFILTER_H_ */
