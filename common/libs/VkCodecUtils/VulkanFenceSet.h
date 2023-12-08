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

#ifndef _VULKANFENCESET_H_
#define _VULKANFENCESET_H_

#include <vector>
#include <atomic>
#include <iostream>
#include "VkCodecUtils/VulkanDeviceContext.h"

class VulkanFenceSet
{
public:
    VulkanFenceSet() : m_vkDevCtx() {}

    VkResult CreateSet(const VulkanDeviceContext* vkDevCtx, uint32_t numFences, VkFenceCreateFlags flags = VkFenceCreateFlags(),
                       const void* pNext = nullptr) {

        DestroySet();

        m_vkDevCtx = vkDevCtx;
        m_fences.resize(numFences);
        const VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, pNext, flags };
        for (uint32_t fenceIdx = 0; fenceIdx < numFences; fenceIdx++ ) {
            VkResult result = m_vkDevCtx->CreateFence(*m_vkDevCtx, &fenceInfo, nullptr, &m_fences[fenceIdx]);
            if (result != VK_SUCCESS) {
                return result;
            }
        }
        return VK_SUCCESS;
    }

    void DestroySet() {
        if (m_vkDevCtx && !m_fences.empty()) {
            for (size_t fenceIdx = 0; fenceIdx < m_fences.size(); fenceIdx++) {
                if (m_fences[fenceIdx] != VK_NULL_HANDLE) {
                    m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_fences[fenceIdx], nullptr);
                    m_fences[fenceIdx] = VK_NULL_HANDLE;
                }
            }
        }
    }

    VkFence GetFence(uint32_t fenceIdx = 0) const {
        if (fenceIdx < m_fences.size()) {
            return m_fences[fenceIdx];
        }
        return VK_NULL_HANDLE;
    }

    virtual ~VulkanFenceSet() {
        DestroySet();
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    std::vector<VkFence>       m_fences;
};

#endif /* _VULKANFENCESET_H_ */
