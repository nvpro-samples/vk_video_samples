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

#ifndef _VULKANCOMMANDBUFFERSSET_H_
#define _VULKANCOMMANDBUFFERSSET_H_

#include <vulkan_interfaces.h>
#include "VkCodecUtils/VulkanDeviceContext.h"

class VulkanCommandBuffersSet {

public:

    VulkanCommandBuffersSet()
        :m_vkDevCtx(),
        m_cmdPool(),
        m_cmdBuffer(1)
        {}

    VkResult CreateCommandBufferPool(const VulkanDeviceContext* vkDevCtx, uint32_t  queueFamilyIndex,
                                     uint32_t maxCommandBuffersCount = 1);

    ~VulkanCommandBuffersSet() {
        DestroyCommandBuffer();
        DestroyCommandBufferPool();
    }

    void DestroyCommandBuffer() {
        if ((m_vkDevCtx != nullptr) && !m_cmdBuffer.empty()) {
            m_vkDevCtx->FreeCommandBuffers(*m_vkDevCtx, m_cmdPool, (uint32_t)m_cmdBuffer.size(), m_cmdBuffer.data());
            m_cmdBuffer.clear();
        }
    }

    void DestroyCommandBufferPool() {
        if (m_cmdPool) {
           m_vkDevCtx->DestroyCommandPool(*m_vkDevCtx, m_cmdPool, nullptr);
           m_cmdPool = VkCommandPool(0);
        }
    }

    VkCommandPool GetCommandPool(uint32_t bufferIndex = 0) {
        return m_cmdPool;
    }

    const VkCommandBuffer* GetCommandBuffer(uint32_t bufferIndex = 0) const {

        if ((size_t)bufferIndex >= m_cmdBuffer.size()) {
            return nullptr;
        }

        return &m_cmdBuffer[bufferIndex];
    }

private:
    const VulkanDeviceContext*   m_vkDevCtx;
    VkCommandPool                m_cmdPool;
    std::vector<VkCommandBuffer> m_cmdBuffer;
};

#endif /* _VULKANCOMMANDBUFFERSSET_H_ */
