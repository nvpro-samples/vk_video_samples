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

#ifndef _VULKANDESCRIPTORSETLAYOUT_H_
#define _VULKANDESCRIPTORSETLAYOUT_H_

#include <sstream>
#include <vulkan_interfaces.h>
#include "VkCodecUtils/Helpers.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/VkBufferResource.h"
#include "VkCodecUtils/VulkanSamplerYcbcrConversion.h"

class VulkanDescriptorSet {

public:

    VulkanDescriptorSet(const VulkanDeviceContext* vkDevCtx = nullptr)
        : m_vkDevCtx(vkDevCtx), m_descPool(), m_descSet() {}

    ~VulkanDescriptorSet()
    {
        DestroyDescriptorSets();
        DestroyDescriptorPool();
    }

    void DestroyDescriptorSets()
    {
        if (m_descSet) {
            m_vkDevCtx->FreeDescriptorSets(*m_vkDevCtx, m_descPool, 1, &m_descSet);
            m_descSet = VkDescriptorSet(0);
        }
    }

    void DestroyDescriptorPool()
    {
        if (m_descPool) {
            m_vkDevCtx->DestroyDescriptorPool(*m_vkDevCtx, m_descPool, nullptr);
            m_descPool = VkDescriptorPool(0);
        }
    }

    VkResult CreateDescriptorPool(const VulkanDeviceContext* vkDevCtx,
                                  uint32_t descriptorCount,
                                  VkDescriptorType descriptorType)
    {

        DestroyDescriptorPool();

        m_vkDevCtx = vkDevCtx;

        VkDescriptorPoolSize descriptorPoolSize = VkDescriptorPoolSize();
        descriptorPoolSize.type = descriptorType;
        descriptorPoolSize.descriptorCount = descriptorCount;

        VkDescriptorPoolCreateInfo descriptor_pool = VkDescriptorPoolCreateInfo();
        descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool.pNext = nullptr;
        descriptor_pool.maxSets = 1;
        descriptor_pool.poolSizeCount = 1;
        descriptor_pool.pPoolSizes = &descriptorPoolSize;
        return m_vkDevCtx->CreateDescriptorPool(*m_vkDevCtx, &descriptor_pool, nullptr, &m_descPool);
    }

    VkResult AllocateDescriptorSets(uint32_t descriptorCount,
                                    VkDescriptorSetLayout* dscLayout)
    {
        assert(m_vkDevCtx);
        DestroyDescriptorSets();

        VkDescriptorSetAllocateInfo alloc_info = VkDescriptorSetAllocateInfo();
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.pNext = nullptr;
        alloc_info.descriptorPool = m_descPool;
        alloc_info.descriptorSetCount = descriptorCount;
        alloc_info.pSetLayouts = dscLayout;
        VkResult result = m_vkDevCtx->AllocateDescriptorSets(*m_vkDevCtx, &alloc_info, &m_descSet);

        return result;
    }

    const VkDescriptorSet* GetDescriptorSet() const {
        return &m_descSet;
    }

private:
    const VulkanDeviceContext* m_vkDevCtx;
    VkDescriptorPool m_descPool;
    VkDescriptorSet  m_descSet;
};

class VulkanDescriptorSetLayoutInfo {

public:

    VulkanDescriptorSetLayoutInfo()
    : m_descriptorSetLayoutBindings(),
      m_descriptorSetLayoutCreateInfo()
    {

    }

    VulkanDescriptorSetLayoutInfo(const std::vector<VkDescriptorSetLayoutBinding>& setLayoutBindings,
                                  VkDescriptorSetLayoutCreateFlags flags)
    : m_descriptorSetLayoutBindings(setLayoutBindings),
      m_descriptorSetLayoutCreateInfo()
    {
        UpdateLayout(flags);
    }

    // A copy points at its own bindings and its own samplers, not at the original's.
    VulkanDescriptorSetLayoutInfo(const VulkanDescriptorSetLayoutInfo& other)
    : m_descriptorSetLayoutBindings(other.m_descriptorSetLayoutBindings),
      m_descriptorSetLayoutCreateInfo()
    {
        UpdateLayout(other.m_descriptorSetLayoutCreateInfo.flags);
    }

    VulkanDescriptorSetLayoutInfo& operator=(const VulkanDescriptorSetLayoutInfo& other)
    {
        if (this != &other) {
            m_descriptorSetLayoutBindings = other.m_descriptorSetLayoutBindings;
            UpdateLayout(other.m_descriptorSetLayoutCreateInfo.flags);
        }
        return *this;
    }

    size_t SetDescriptorSetLayoutInfo(const std::vector<VkDescriptorSetLayoutBinding>& setLayoutBindings,
                                      VkDescriptorSetLayoutCreateFlags flags)
    {
        m_descriptorSetLayoutBindings = setLayoutBindings;
        return UpdateLayout(flags);
    }

    const VkDescriptorSetLayoutCreateInfo* GetCreateInfo() const {
        return &m_descriptorSetLayoutCreateInfo;
    }

    VkDescriptorSetLayoutCreateFlags GetDescriptorLayoutMode() const {
        // VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR or
        // VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
        return m_descriptorSetLayoutCreateInfo.flags;
    }

private:
    // Vulkan reads pImmutableSamplers only for the two sampler types, and then descriptorCount of them.
    static bool HasImmutableSamplers(const VkDescriptorSetLayoutBinding& binding) {
        return (binding.pImmutableSamplers != nullptr) && (binding.descriptorCount > 0) &&
               ((binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
                (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
    }

    size_t UpdateLayout(VkDescriptorSetLayoutCreateFlags flags) {

        // Copy the immutable samplers, all of them, before pointing any binding at the copies:
        // a pointer into a vector that is still growing dangles when the vector reallocates.
        // m_immutableSamplers is replaced only once every sampler has been read, since the
        // bindings may point into it (a layout set again from its own create info).
        std::vector<VkSampler> immutableSamplers;
        for (const auto& descriptorSetLayoutBinding : m_descriptorSetLayoutBindings) {
            if (HasImmutableSamplers(descriptorSetLayoutBinding)) {
                immutableSamplers.insert(immutableSamplers.end(),
                                         descriptorSetLayoutBinding.pImmutableSamplers,
                                         descriptorSetLayoutBinding.pImmutableSamplers +
                                             descriptorSetLayoutBinding.descriptorCount);
            }
        }
        m_immutableSamplers = std::move(immutableSamplers);

        const VkSampler* pNextImmutableSampler = m_immutableSamplers.data();
        for (auto& descriptorSetLayoutBinding : m_descriptorSetLayoutBindings) {
            if (HasImmutableSamplers(descriptorSetLayoutBinding)) {
                descriptorSetLayoutBinding.pImmutableSamplers = pNextImmutableSampler;
                pNextImmutableSampler += descriptorSetLayoutBinding.descriptorCount;
            } else {
                descriptorSetLayoutBinding.pImmutableSamplers = nullptr;
            }
        }

        m_descriptorSetLayoutCreateInfo = VkDescriptorSetLayoutCreateInfo();
        m_descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        m_descriptorSetLayoutCreateInfo.flags = flags;
        m_descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(m_descriptorSetLayoutBindings.size());
        m_descriptorSetLayoutCreateInfo.pBindings = m_descriptorSetLayoutBindings.data();

        return m_descriptorSetLayoutBindings.size();
    }
private:
    std::vector<VkDescriptorSetLayoutBinding> m_descriptorSetLayoutBindings;
    std::vector<VkSampler>                    m_immutableSamplers;
    VkDescriptorSetLayoutCreateInfo           m_descriptorSetLayoutCreateInfo;
};

class VulkanDescriptorSetLayout {

    enum { MAX_DESCRIPTOR_SET_POOLS = 4 };

public:
    VulkanDescriptorSetLayout()
     : m_vkDevCtx()
     , m_descriptorSetLayoutInfo()
     , m_dscLayout()
     , m_pipelineLayout()
     , m_currentDescriptorSetPools(-1)
     , m_descSets{}
     , m_maxNumFrames(0)
     , m_descriptorLayoutSize(0)
     , m_descriptorBufferSize(0)
     , m_descriptorBufferProperties()
    { }

    ~VulkanDescriptorSetLayout()
    {
        DestroyPipelineLayout();
        DestroyDescriptorSetLayout();
    }

    void DestroyPipelineLayout()
    {
        if (m_pipelineLayout) {
            m_vkDevCtx->DestroyPipelineLayout(*m_vkDevCtx, m_pipelineLayout, nullptr);
            m_pipelineLayout = VkPipelineLayout(0);
        }
    }

    void DestroyDescriptorSetLayout()
    {
        m_resourceDescriptorBuffer = nullptr;

        if (m_dscLayout) {
            m_vkDevCtx->DestroyDescriptorSetLayout(*m_vkDevCtx, m_dscLayout, nullptr);
            m_dscLayout = VkDescriptorSetLayout(0);
        }
    }

    // initialize descriptor set
    VkResult CreateDescriptorSet(const VulkanDeviceContext* vkDevCtx,
                                 const std::vector<VkDescriptorSetLayoutBinding>& descriptorSetLayoutBindings,
                                 VkDescriptorSetLayoutCreateFlags descriptorSetLayoutCreateFlags = 0,
                                 uint32_t                   pushConstantRangeCount = 0,
                                 const VkPushConstantRange* pPushConstantRanges = nullptr,
                                 const VulkanSamplerYcbcrConversion* pSamplerYcbcrConversion = nullptr,
                                 uint32_t maxNumFrames = 1,
                                 bool autoSelectdescriptorSetLayoutCreateFlags = true);

    // initialize descriptor set
    VkResult CreateDescriptorSet(const VulkanDeviceContext* vkDevCtx, VkDescriptorPool allocPool, VkDescriptorSetLayout* dscLayouts, uint32_t descriptorSetCount = 1);

    VkResult WriteDescriptorSet(uint32_t descriptorWriteCount,
                                const VkWriteDescriptorSet* pDescriptorWrites);

    VkResult CreateFragmentShaderOutput(VkDescriptorType outMode, uint32_t outSet, uint32_t outBinding, uint32_t outArrayIndex, std::stringstream& imageFss);

    VkResult CreateFragmentShaderLayouts(const uint32_t* setIds, uint32_t numSets, std::stringstream& texFss);


    const VkDescriptorSet* GetDescriptorSet() const {
        if (m_currentDescriptorSetPools < 0) {
            return nullptr;
        }
        return m_descSets[m_currentDescriptorSetPools].GetDescriptorSet();
    }


    bool UsesDescriptorBuffer() const {
        return m_resourceDescriptorBuffer != nullptr;
    }

    VkDeviceOrHostAddressConstKHR UpdateDescriptorBuffer(uint32_t bufferIdx, uint32_t set,
                                                         uint32_t descriptorWriteCount,
                                                         const VkWriteDescriptorSet* pDescriptorWrites) const {

        if (!(bufferIdx < m_maxNumFrames)) {
            assert(!"bufferIdx is out of range!");
            return VkDeviceOrHostAddressConstKHR();
        }

        const VkDeviceSize bufferIdxOffset = (bufferIdx * m_descriptorLayoutSize);
        assert(bufferIdxOffset < m_descriptorBufferSize);

        // Set descriptors for image
        VkDeviceSize dstBufferOffset = 0;
        VkDeviceSize maxSize = 0;
        uint8_t* imageDescriptorBufPtr = const_cast<uint8_t*>((const uint8_t*)m_resourceDescriptorBuffer->GetDataPtr(dstBufferOffset, maxSize));
        assert(imageDescriptorBufPtr);
        imageDescriptorBufPtr += bufferIdxOffset;

        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            const VkWriteDescriptorSet* pDescriptorWrite = &pDescriptorWrites[i];

            VkDeviceSize dstBindingOffset = 0;
            m_vkDevCtx->GetDescriptorSetLayoutBindingOffsetEXT(*m_vkDevCtx, m_dscLayout,
                                                               pDescriptorWrite->dstBinding,
                                                               &dstBindingOffset);

            VkDescriptorGetInfoEXT descriptorInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptorInfo.type = pDescriptorWrite->descriptorType;
            size_t descriptorSize = 0;
            switch (pDescriptorWrite->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                descriptorSize = m_descriptorBufferProperties.samplerDescriptorSize;
                descriptorInfo.data.pSampler = &pDescriptorWrite->pImageInfo->sampler;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                descriptorSize = m_descriptorBufferProperties.combinedImageSamplerDescriptorSize;
                descriptorInfo.data.pCombinedImageSampler = pDescriptorWrite->pImageInfo;
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                descriptorSize = m_descriptorBufferProperties.sampledImageDescriptorSize;
                descriptorInfo.data.pSampledImage = pDescriptorWrite->pImageInfo;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                descriptorSize = m_descriptorBufferProperties.storageImageDescriptorSize;
                descriptorInfo.data.pStorageImage = pDescriptorWrite->pImageInfo;
                break;
            default:
                assert(!"Unknown descriptor type");
                continue;
            }

            // The binding's descriptor may end exactly where the layout does.
            assert((dstBindingOffset + descriptorSize) <= m_descriptorLayoutSize);

            m_vkDevCtx->GetDescriptorEXT(*m_vkDevCtx, &descriptorInfo, descriptorSize,
                                         imageDescriptorBufPtr + dstBindingOffset);
        }
        VkBuffer imageDescriptorBuffer = m_resourceDescriptorBuffer->GetBuffer();
        assert(imageDescriptorBuffer);
        VkDeviceOrHostAddressConstKHR imageDescriptorBufferDeviceAddress;
        VkBufferDeviceAddressInfoKHR bufferDeviceAddressInfo{};
        bufferDeviceAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bufferDeviceAddressInfo.buffer = imageDescriptorBuffer;
        imageDescriptorBufferDeviceAddress.deviceAddress = m_vkDevCtx->GetBufferDeviceAddressKHR(*m_vkDevCtx, &bufferDeviceAddressInfo);
        imageDescriptorBufferDeviceAddress.deviceAddress += bufferIdxOffset;

        return imageDescriptorBufferDeviceAddress;
    }


    VulkanDescriptorSet* GetNextDescriptorSet() {
        m_currentDescriptorSetPools++;
        m_currentDescriptorSetPools = m_currentDescriptorSetPools % MAX_DESCRIPTOR_SET_POOLS;
        assert((m_currentDescriptorSetPools >= 0) && (m_currentDescriptorSetPools < MAX_DESCRIPTOR_SET_POOLS));
        return &m_descSets[m_currentDescriptorSetPools];
    }

    const VkDescriptorSetLayout* GetDescriptorSetLayout() const {
        return &m_dscLayout;
    }

    VkPipelineLayout GetPipelineLayout() const {
        return m_pipelineLayout;
    }

    const VulkanDescriptorSetLayoutInfo& GetDescriptorSetLayoutInfo() const {
        return m_descriptorSetLayoutInfo;
    }

private:
    const VulkanDeviceContext*         m_vkDevCtx;
    VulkanDescriptorSetLayoutInfo      m_descriptorSetLayoutInfo;
    VkDescriptorSetLayout              m_dscLayout;
    VkPipelineLayout                   m_pipelineLayout;
    int32_t                            m_currentDescriptorSetPools;
    VulkanDescriptorSet                m_descSets[MAX_DESCRIPTOR_SET_POOLS];
    // If VK_EXT_descriptor_buffer is supported:
    uint32_t                           m_maxNumFrames;
    VkDeviceSize                       m_descriptorLayoutSize;
    VkDeviceSize                       m_descriptorBufferSize;
    VkPhysicalDeviceDescriptorBufferPropertiesEXT
                                       m_descriptorBufferProperties;
    VkSharedBaseObj<VkBufferResource>  m_resourceDescriptorBuffer;
};

#endif /* _VULKANDESCRIPTORSETLAYOUT_H_ */
