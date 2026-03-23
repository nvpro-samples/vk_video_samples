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

#ifndef _VULKANVIDEOREFERENCECOUNTEDPOOL_H_
#define _VULKANVIDEOREFERENCECOUNTEDPOOL_H_

#include <vector>
#include <bitset>
#include <algorithm>
#include <mutex>
#include "VkCodecUtils/VkVideoRefCountBase.h"


template <class RefCountedNodeType, const size_t MAX_POOL_ENTRIES>
class VulkanVideoRefCountedPool {

public:
    class VulkanVideoRefCountedPoolIterator {

        bool VisitNode(VkSharedBaseObj<RefCountedNodeType>& node,
                       uint32_t index, bool isValid, bool isAvailable);
    };

    VulkanVideoRefCountedPool(uint32_t maxNodes = 32)
    : m_poolMutex()
    , m_maxNodes(maxNodes)
    , m_poolNodeSlotsInUseMask()
    , m_poolNodesCheckedOutMask()
    , m_pool(MAX_POOL_ENTRIES) { }

    virtual ~VulkanVideoRefCountedPool()
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        uint32_t maxNodes = m_maxNodes;
        m_maxNodes = 0;
        for (uint32_t i = 0; i < maxNodes; i++) {
            m_pool[i] = nullptr;
            m_poolNodesCheckedOutMask[i] = false;
        }
    }

    uint32_t SetMaxNodes(uint32_t maxNodes)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);

        m_maxNodes = std::min<uint32_t>(MAX_POOL_ENTRIES, maxNodes);
        return m_maxNodes;
    }

    uint32_t GetMaxNodes()
    {
        return m_maxNodes;
    }

    uint32_t VisitNodes(VulkanVideoRefCountedPoolIterator* it)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);

        for (uint32_t i = 0; i < m_maxNodes; i++) {
            bool isValid = (m_pool[i] != nullptr);
            bool isAvailable = isValid && !m_poolNodesCheckedOutMask[i];
            it->VisitNode(m_pool[i], i, isValid, isAvailable);
        }
        return m_maxNodes;
    }

    uint32_t GetAvailableNodesNumber()
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);

        uint32_t numFreeNodes = 0;
        for (uint32_t i = 0; i < m_maxNodes; i++) {
            if (m_pool[i] && !m_poolNodesCheckedOutMask[i]) {
                numFreeNodes++;
            }
        }
        return numFreeNodes;
    }

    int32_t GetAvailableNodeFromPool(VkSharedBaseObj<RefCountedNodeType>& availableNodeFromPool)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        int32_t idx = GetAvailableNodeIndx();
        if (idx >= 0) {
            assert((uint32_t)idx < m_maxNodes);
            auto* pool = this;
            uint32_t uidx = static_cast<uint32_t>(idx);
            struct CheckoutGuard {
                VkSharedBaseObj<RefCountedNodeType> node;
                VulkanVideoRefCountedPool* pool;
                uint32_t idx;
                ~CheckoutGuard() { if (pool) pool->ReturnNodeToPool(idx); }
            };
            auto guard = std::make_shared<CheckoutGuard>();
            guard->node = m_pool[idx];
            guard->pool = pool;
            guard->idx = uidx;
            availableNodeFromPool = std::shared_ptr<RefCountedNodeType>(guard, guard->node.get());
        }
        return idx;
    }

    uint32_t GetFreeNodesNumber()
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        return m_maxNodes - (uint32_t)m_poolNodeSlotsInUseMask.count();
    }

    int32_t AddNodeToPool(VkSharedBaseObj<RefCountedNodeType>& newNodeToPool, bool setUnavailable)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        int32_t freeNodeSlotIndx = GetFreeNodeSlotIndx();
        if (freeNodeSlotIndx >= 0) {
            assert((uint32_t)freeNodeSlotIndx < m_maxNodes);
            m_poolNodesCheckedOutMask[freeNodeSlotIndx] = setUnavailable;
            m_pool[freeNodeSlotIndx] = newNodeToPool;
        }
        return freeNodeSlotIndx;
    }

private:
    void ReturnNodeToPool(uint32_t idx)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        assert(idx < m_maxNodes);
        assert(m_poolNodesCheckedOutMask[idx]);
        m_poolNodesCheckedOutMask[idx] = false;
    }

    int32_t GetAvailableNodeIndx(bool setUnavailable = true) {
        for (uint32_t i = 0; i < m_maxNodes; i++) {
            if (m_pool[i] && !m_poolNodesCheckedOutMask[i]) {
                if (setUnavailable) {
                    m_poolNodesCheckedOutMask[i] = true;
                }
                return i;
            }
        }
        return -1;
    }

    int32_t GetFreeNodeSlotIndx(bool allocate = true) {

        if (!(m_poolNodeSlotsInUseMask.count() < m_maxNodes)) {
            return -1;
        }

        for (uint32_t i = 0; i < m_maxNodes; i++) {
            if (m_poolNodeSlotsInUseMask[i] == false) {
                if (allocate) {
                    m_poolNodeSlotsInUseMask[i] = true;
                }
                return i;
            }
        }
        return -1;
    }

private:
    std::mutex                                         m_poolMutex;
    uint32_t                                           m_maxNodes;
    std::bitset<MAX_POOL_ENTRIES>                      m_poolNodeSlotsInUseMask;
    std::bitset<MAX_POOL_ENTRIES>                      m_poolNodesCheckedOutMask;
    std::vector<VkSharedBaseObj<RefCountedNodeType>>   m_pool;
};

#endif // _VULKANVIDEOREFERENCECOUNTEDPOOL_H_
