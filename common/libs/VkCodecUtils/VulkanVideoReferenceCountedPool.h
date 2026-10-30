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

#include <cassert>
#include <cstdint>
#include <vector>
#include <bitset>
#include <algorithm>
#include <memory>
#include <mutex>
#include "VkCodecUtils/VkVideoRefCountBase.h"


template <class RefCountedNodeType, const size_t MAX_POOL_ENTRIES>
class VulkanVideoRefCountedPool {

public:
    class VulkanVideoRefCountedPoolIterator {

        bool VisitNode(VkSharedBaseObj<RefCountedNodeType>& node,
                       uint32_t index, bool isValid, bool isAvailable);
    };

    VulkanVideoRefCountedPool(uint32_t maxNodes = MAX_POOL_ENTRIES)
    : m_returnPath(std::make_shared<ReturnPath>(this))
    , m_poolMutex()
    , m_maxNodes(maxNodes)
    , m_poolNodeSlotsInUseMask()
    , m_poolNodesCheckedOutMask()
    , m_pool(MAX_POOL_ENTRIES) { }

    virtual ~VulkanVideoRefCountedPool()
    {
        {
            // A node checked out now returns to nothing when its holder releases it.
            std::lock_guard<std::mutex> lock(m_returnPath->mutex);
            m_returnPath->pool = nullptr;
        }
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
            availableNodeFromPool = CheckOutNode(static_cast<uint32_t>(idx));
        }
        return idx;
    }

    uint32_t GetFreeNodesNumber()
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        return m_maxNodes - (uint32_t)m_poolNodeSlotsInUseMask.count();
    }

    // With setUnavailable, the new node is checked out to the caller: newNodeToPool
    // is replaced by a reference that returns the node to the pool when its last
    // copy is released, as a reference from GetAvailableNodeFromPool does.
    int32_t AddNodeToPool(VkSharedBaseObj<RefCountedNodeType>& newNodeToPool, bool setUnavailable)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        int32_t freeNodeSlotIndx = GetFreeNodeSlotIndx();
        if (freeNodeSlotIndx >= 0) {
            assert((uint32_t)freeNodeSlotIndx < m_maxNodes);
            m_poolNodesCheckedOutMask[freeNodeSlotIndx] = setUnavailable;
            m_pool[freeNodeSlotIndx] = newNodeToPool;
            if (setUnavailable) {
                newNodeToPool = CheckOutNode(static_cast<uint32_t>(freeNodeSlotIndx));
            }
        }
        return freeNodeSlotIndx;
    }

private:
    // The way back to the pool for the nodes it checks out. The checkout guards share it, so it
    // outlives a pool destroyed while a node is out, and the pool clears it first.
    struct ReturnPath {
        explicit ReturnPath(VulkanVideoRefCountedPool* p) : pool(p) {}
        std::mutex mutex;
        VulkanVideoRefCountedPool* pool;
    };

    // Called with m_poolMutex held, for a node already marked checked out. The
    // reference it returns shares a guard whose destructor marks the node
    // available again, if the pool is still there.
    VkSharedBaseObj<RefCountedNodeType> CheckOutNode(uint32_t idx)
    {
        struct CheckoutGuard {
            VkSharedBaseObj<RefCountedNodeType> node;
            std::shared_ptr<ReturnPath> returnPath;
            uint32_t idx;
            ~CheckoutGuard()
            {
                std::lock_guard<std::mutex> lock(returnPath->mutex);
                if (returnPath->pool) {
                    returnPath->pool->ReturnNodeToPool(idx);
                }
            }
        };
        auto guard = std::make_shared<CheckoutGuard>();
        guard->node = m_pool[idx];
        guard->returnPath = m_returnPath;
        guard->idx = idx;
        return VkSharedBaseObj<RefCountedNodeType>(guard, guard->node.get());
    }

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
    std::shared_ptr<ReturnPath>                        m_returnPath;
    std::mutex                                         m_poolMutex;
    uint32_t                                           m_maxNodes;
    std::bitset<MAX_POOL_ENTRIES>                      m_poolNodeSlotsInUseMask;
    std::bitset<MAX_POOL_ENTRIES>                      m_poolNodesCheckedOutMask;
    std::vector<VkSharedBaseObj<RefCountedNodeType>>   m_pool;
};

#endif // _VULKANVIDEOREFERENCECOUNTEDPOOL_H_
