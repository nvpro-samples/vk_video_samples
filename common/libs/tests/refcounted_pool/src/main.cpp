/*
 * Copyright 2026 NVIDIA Corporation.
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

/*
 * VulkanVideoRefCountedPool checkout and return, without a device.
 *
 * The decoder and the encoder fetch their bitstream buffers the same way: a
 * free pool node if there is one, otherwise a new buffer added to the pool
 * checked out (AddNodeToPool(buffer, true)). Every buffer, pooled or new, must
 * come back to the pool when its last user releases it. Otherwise each picture
 * that finds no free node creates a buffer, and once the pool is full, the
 * buffer is freed again after its picture.
 */

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>

#include "VkCodecUtils/VulkanVideoReferenceCountedPool.h"

namespace {

struct Node : public VkVideoRefCountBase {
    explicit Node(uint32_t id) : m_id(id) {}
    uint32_t m_id;
};

constexpr size_t kPoolCapacity = 64;
using Pool = VulkanVideoRefCountedPool<Node, kPoolCapacity>;

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

// GetBitstreamBuffer as VkVideoDecoder and VkVideoEncoder implement it.
VkSharedBaseObj<Node> GetBuffer(Pool& pool, uint32_t& created, uint32_t& addFailures) {
    VkSharedBaseObj<Node> node;
    if (pool.GetAvailableNodeFromPool(node) >= 0) {
        return node;
    }
    node = std::make_shared<Node>(created++);
    if (pool.AddNodeToPool(node, true) < 0) {
        addFailures++;
    }
    return node;
}

// A node added checked out stays out while any copy of the caller's reference
// lives, then becomes the pool's next available node.
void AddedNodeReturnsToPool() {
    Pool pool;
    VkSharedBaseObj<Node> node = std::make_shared<Node>(7);
    Node* const added = node.get();
    CHECK(pool.AddNodeToPool(node, true) == 0);
    CHECK(node.get() == added);
    CHECK(pool.GetAvailableNodesNumber() == 0);

    VkSharedBaseObj<Node> copy = node;
    node = nullptr;
    CHECK(pool.GetAvailableNodesNumber() == 0);
    copy = nullptr;
    CHECK(pool.GetAvailableNodesNumber() == 1);

    VkSharedBaseObj<Node> again;
    CHECK(pool.GetAvailableNodeFromPool(again) == 0);
    CHECK(again.get() == added);
    CHECK(pool.GetAvailableNodesNumber() == 0);
}

// A node added available can be checked out at once.
void PreallocatedNodeIsAvailable() {
    Pool pool;
    VkSharedBaseObj<Node> node = std::make_shared<Node>(1);
    Node* const added = node.get();
    CHECK(pool.AddNodeToPool(node, false) == 0);
    node = nullptr;
    CHECK(pool.GetAvailableNodesNumber() == 1);

    VkSharedBaseObj<Node> out;
    CHECK(pool.GetAvailableNodeFromPool(out) == 0);
    CHECK(out.get() == added);
}

// The decoder's pattern: 8 nodes preallocated, then 10000 pictures with 20
// buffers held at a time, as the frame buffer slots hold them. The 21st
// buffer is fetched while the 20 are held, so the pool needs 21 nodes, 13 of
// them created, and then creates no more.
void SteadyStateCreatesNoNodes() {
    constexpr uint32_t kPreallocated = 8;
    constexpr size_t kHeld = 20;
    constexpr uint32_t kPictures = 10000;

    Pool pool;
    for (uint32_t i = 0; i < kPreallocated; i++) {
        VkSharedBaseObj<Node> node = std::make_shared<Node>(1000 + i);
        CHECK(pool.AddNodeToPool(node, false) == static_cast<int32_t>(i));
    }

    uint32_t created = 0;
    uint32_t addFailures = 0;
    std::deque<VkSharedBaseObj<Node>> held;
    for (uint32_t picture = 0; picture < kPictures; picture++) {
        held.push_back(GetBuffer(pool, created, addFailures));
        if (held.size() > kHeld) {
            held.pop_front();
        }
    }

    std::printf("  %u pictures, %zu buffers held: %u nodes created, %u not added to the pool\n", kPictures, kHeld, created,
                addFailures);
    CHECK(created == kHeld + 1 - kPreallocated);
    CHECK(addFailures == 0);

    held.clear();
    CHECK(pool.GetAvailableNodesNumber() == kHeld + 1);
}

// A pool built with the default size holds as many nodes as its template
// capacity. The decoder keeps one bitstream buffer per frame buffer slot (up
// to VulkanVideoFrameBuffer::maxImages, 32) and the parser holds two more while
// it swaps buffers, so its 64-entry pool must not stop at 32.
void DefaultSizeIsTemplateCapacity() {
    Pool pool;
    CHECK(pool.GetMaxNodes() == kPoolCapacity);
    uint32_t added = 0;
    for (uint32_t i = 0; i < kPoolCapacity; i++) {
        VkSharedBaseObj<Node> node = std::make_shared<Node>(i);
        if (pool.AddNodeToPool(node, false) == static_cast<int32_t>(i)) {
            added++;
        }
    }
    CHECK(added == kPoolCapacity);
    VkSharedBaseObj<Node> extra = std::make_shared<Node>(static_cast<uint32_t>(kPoolCapacity));
    CHECK(pool.AddNodeToPool(extra, false) < 0);
}

}  // namespace

int main() {
    struct {
        const char* name;
        void (*run)();
    } const tests[] = {
        {"AddedNodeReturnsToPool", AddedNodeReturnsToPool},
        {"PreallocatedNodeIsAvailable", PreallocatedNodeIsAvailable},
        {"SteadyStateCreatesNoNodes", SteadyStateCreatesNoNodes},
        {"DefaultSizeIsTemplateCapacity", DefaultSizeIsTemplateCapacity},
    };

    for (const auto& test : tests) {
        const int before = g_failures;
        std::printf("%s\n", test.name);
        test.run();
        std::printf("  %s\n", (g_failures == before) ? "PASS" : "FAIL");
    }
    std::printf("%s: %d failed expectation(s)\n", (g_failures == 0) ? "PASS" : "FAIL", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
