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
 * VulkanDescriptorSetLayoutInfo's immutable samplers, without a device.
 *
 * The layout keeps its own copy of the bindings and of their immutable samplers, because its
 * callers pass the address of a local VkSampler and the create info is used after they return.
 * Every sampler of every sampler binding must be copied, and every binding must point at its own
 * copies for as long as the layout lives, whatever happens to the caller's arrays. A copy of the
 * layout must point at the copy's samplers.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "VkCodecUtils/VulkanDescriptorSetLayout.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

// A handle value that is only compared, never passed to Vulkan. VkSampler is a pointer on 64-bit
// targets and a uint64_t on 32-bit ones; both are 8 bytes.
VkSampler FakeSampler(uint64_t value) {
    static_assert(sizeof(VkSampler) == sizeof(value), "a non-dispatchable handle is 64 bits");
    VkSampler sampler = VK_NULL_HANDLE;
    std::memcpy(&sampler, &value, sizeof(sampler));
    return sampler;
}

VkDescriptorSetLayoutBinding Binding(uint32_t binding, VkDescriptorType type, uint32_t count, const VkSampler* samplers) {
    return VkDescriptorSetLayoutBinding{binding, type, count, VK_SHADER_STAGE_COMPUTE_BIT, samplers};
}

// The layout of the two-sampler case: a combined image sampler with 2 immutable samplers, a
// storage image, and a sampler binding with 3. The caller's sampler values start at base.
struct TwoSamplerBindings {
    explicit TwoSamplerBindings(uint64_t base)
        : combined{FakeSampler(base + 1), FakeSampler(base + 2)},
          samplers{FakeSampler(base + 3), FakeSampler(base + 4), FakeSampler(base + 5)},
          bindings{Binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, combined),
                   Binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, nullptr), Binding(2, VK_DESCRIPTOR_TYPE_SAMPLER, 3, samplers)} {}

    // Overwrite the caller's samplers, as a caller's stack does once it returns.
    void Scribble() {
        for (VkSampler& sampler : combined) {
            sampler = FakeSampler(0xdead);
        }
        for (VkSampler& sampler : samplers) {
            sampler = FakeSampler(0xdead);
        }
    }

    VkSampler combined[2];
    VkSampler samplers[3];
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

// The create info holds the three bindings, the first with the samplers base + 1 and base + 2,
// the third with base + 3 .. base + 5, none of them in the caller's arrays.
void CheckTwoSamplerLayout(const VulkanDescriptorSetLayoutInfo& info, uint64_t base, const TwoSamplerBindings* caller) {
    const VkDescriptorSetLayoutCreateInfo* createInfo = info.GetCreateInfo();
    CHECK(createInfo->bindingCount == 3);
    if (createInfo->bindingCount != 3) {
        return;
    }
    const VkDescriptorSetLayoutBinding* bindings = createInfo->pBindings;

    CHECK(bindings[0].descriptorCount == 2);
    CHECK(bindings[0].pImmutableSamplers != nullptr);
    if (bindings[0].pImmutableSamplers != nullptr) {
        CHECK(bindings[0].pImmutableSamplers[0] == FakeSampler(base + 1));
        CHECK(bindings[0].pImmutableSamplers[1] == FakeSampler(base + 2));
    }

    CHECK(bindings[1].pImmutableSamplers == nullptr);

    CHECK(bindings[2].descriptorCount == 3);
    CHECK(bindings[2].pImmutableSamplers != nullptr);
    if (bindings[2].pImmutableSamplers != nullptr) {
        CHECK(bindings[2].pImmutableSamplers[0] == FakeSampler(base + 3));
        CHECK(bindings[2].pImmutableSamplers[1] == FakeSampler(base + 4));
        CHECK(bindings[2].pImmutableSamplers[2] == FakeSampler(base + 5));
    }

    if (caller != nullptr) {
        CHECK(bindings != caller->bindings.data());
        CHECK(bindings[0].pImmutableSamplers != caller->combined);
        CHECK(bindings[2].pImmutableSamplers != caller->samplers);
    }
}

// Two bindings with 2 and 3 immutable samplers: all five are copied, and both bindings keep
// pointing at their copies, after the caller's arrays change and its binding vector is gone.
void TwoSamplerBindingsAreCopied() {
    auto caller = std::make_unique<TwoSamplerBindings>(0x100);
    VulkanDescriptorSetLayoutInfo info(caller->bindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    CHECK(info.GetDescriptorLayoutMode() == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    CheckTwoSamplerLayout(info, 0x100, caller.get());
    caller->Scribble();
    CheckTwoSamplerLayout(info, 0x100, nullptr);
    caller.reset();
    CheckTwoSamplerLayout(info, 0x100, nullptr);
}

// SetDescriptorSetLayoutInfo replaces the layout, the samplers with it.
void SetReplacesTheSamplers() {
    TwoSamplerBindings first(0x200);
    VulkanDescriptorSetLayoutInfo info(first.bindings, 0);

    auto second = std::make_unique<TwoSamplerBindings>(0x300);
    CHECK(info.SetDescriptorSetLayoutInfo(second->bindings, 0) == 3);
    second.reset();
    CheckTwoSamplerLayout(info, 0x300, nullptr);
}

// A layout set again from its own create info: the bindings it is given point at its own samplers,
// which it reads before it replaces them.
void SetFromItsOwnCreateInfo() {
    TwoSamplerBindings caller(0x400);
    VulkanDescriptorSetLayoutInfo info(caller.bindings, 0);
    const VkDescriptorSetLayoutCreateInfo* createInfo = info.GetCreateInfo();
    const std::vector<VkDescriptorSetLayoutBinding> own(createInfo->pBindings, createInfo->pBindings + createInfo->bindingCount);
    CHECK(info.SetDescriptorSetLayoutInfo(own, 0) == 3);
    CheckTwoSamplerLayout(info, 0x400, nullptr);
}

// A copy, by construction or by assignment, points at its own bindings and samplers, and stays
// valid after the original is destroyed.
void CopyOwnsItsSamplers() {
    TwoSamplerBindings caller(0x500);
    auto original =
        std::make_unique<VulkanDescriptorSetLayoutInfo>(caller.bindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

    VulkanDescriptorSetLayoutInfo copy(*original);
    VulkanDescriptorSetLayoutInfo assigned;
    assigned = *original;
    CHECK(copy.GetCreateInfo()->pBindings != original->GetCreateInfo()->pBindings);
    CHECK(assigned.GetCreateInfo()->pBindings != original->GetCreateInfo()->pBindings);
    original.reset();

    CHECK(copy.GetDescriptorLayoutMode() == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    CHECK(assigned.GetDescriptorLayoutMode() == VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    CheckTwoSamplerLayout(copy, 0x500, nullptr);
    CheckTwoSamplerLayout(assigned, 0x500, nullptr);
}

// Vulkan ignores pImmutableSamplers for a binding that is not a sampler type, and for one with no
// descriptors. The layout reads nothing through such a pointer and keeps none of the caller's.
void IgnoredSamplerPointersAreDropped() {
    VkSampler unused[1] = {FakeSampler(0x600)};
    const std::vector<VkDescriptorSetLayoutBinding> bindings{
        Binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4, unused),
        Binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, unused),
        Binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, unused),
    };
    VulkanDescriptorSetLayoutInfo info(bindings, 0);
    const VkDescriptorSetLayoutCreateInfo* createInfo = info.GetCreateInfo();
    CHECK(createInfo->bindingCount == 3);
    for (uint32_t i = 0; i < createInfo->bindingCount; i++) {
        CHECK(createInfo->pBindings[i].pImmutableSamplers == nullptr);
    }
}

}  // namespace

int main() {
    struct {
        const char* name;
        void (*run)();
    } const tests[] = {
        {"TwoSamplerBindingsAreCopied", TwoSamplerBindingsAreCopied},
        {"SetReplacesTheSamplers", SetReplacesTheSamplers},
        {"SetFromItsOwnCreateInfo", SetFromItsOwnCreateInfo},
        {"CopyOwnsItsSamplers", CopyOwnsItsSamplers},
        {"IgnoredSamplerPointersAreDropped", IgnoredSamplerPointersAreDropped},
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
