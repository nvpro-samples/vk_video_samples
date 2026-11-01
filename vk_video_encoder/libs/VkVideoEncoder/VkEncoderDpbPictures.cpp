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

#include "VkVideoEncoder/VkEncoderDpbPictures.h"

VvsDpbResources VkEncoderDpbPictures::Resources(const VkSharedBaseObj<VulkanVideoImagePool>* pPool) {
    m_pPool = pPool;
    VvsDpbResources resources{};
    resources.mode = VVS_DPB_RESOURCE_MODE_CALLBACK;
    resources.callbacks.pUserData = this;
    resources.callbacks.pfnAcquireResource = Acquire;
    resources.callbacks.pfnRetainResource = Retain;
    resources.callbacks.pfnReleaseResource = Release;
    return resources;
}

VkResult VKAPI_CALL VkEncoderDpbPictures::Acquire(void* pUserData, const VvsDpbPictureInfo* pPictureInfo, uint32_t* pResourceIndex,
                                                  const VkVideoPictureResourceInfoKHR** ppResource) {
    (void)pPictureInfo;
    VkEncoderDpbPictures* pPictures = static_cast<VkEncoderDpbPictures*>(pUserData);
    VkSharedBaseObj<VulkanVideoImagePoolNode> image;
    if ((pPictures->m_pPool == nullptr) || (*pPictures->m_pPool == nullptr) || (pPictures->m_pFrame == nullptr) ||
        !(*pPictures->m_pPool)->GetAvailableImage(image, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR) || (image == nullptr) ||
        (image->GetImageIndex() < 0)) {
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    // The picture is reconstructed at the coded offset and extent of its input.
    VkVideoPictureResourceInfoKHR* pResource = image->GetPictureResourceInfo();
    pResource->codedOffset = pPictures->m_pFrame->encodeInfo.srcPictureResource.codedOffset;
    pResource->codedExtent = pPictures->m_pFrame->encodeInfo.srcPictureResource.codedExtent;
    const uint32_t index = static_cast<uint32_t>(image->GetImageIndex());
    if (index >= pPictures->m_pictures.size()) {
        pPictures->m_pictures.resize(index + 1);
    }
    pPictures->m_pictures[index] = image;
    *pResourceIndex = index;
    *ppResource = pResource;
    return VK_SUCCESS;
}

// A rewind brings a released picture back. The encoders do not rewind (their contexts keep no
// history), so only a picture released in the call in progress can come back, and its image is still
// held.
void VKAPI_CALL VkEncoderDpbPictures::Retain(void* pUserData, uint32_t resourceIndex) {
    std::vector<uint32_t>& releases = static_cast<VkEncoderDpbPictures*>(pUserData)->m_releases;
    for (auto it = releases.begin(); it != releases.end(); ++it) {
        if (*it == resourceIndex) {
            releases.erase(it);
            return;
        }
    }
    assert(!"VkEncoderDpbPictures::Retain: the image of a released picture is gone");
}

void VKAPI_CALL VkEncoderDpbPictures::Release(void* pUserData, uint32_t resourceIndex) {
    static_cast<VkEncoderDpbPictures*>(pUserData)->m_releases.push_back(resourceIndex);
}

void VkEncoderDpbPictures::EndPicture(VkVideoEncoder::VkVideoEncodeFrameInfo* pFrame, const VvsDpbPictureSlots* pSlots) {
    m_pFrame = nullptr;
    if (pSlots != nullptr) {
        assert(pSlots->referenceSlotCount <= ARRAYSIZE(pFrame->dpbImageResources));
        assert(pFrame->numDpbImageResources == 0);
        for (uint32_t i = 0; i < pSlots->referenceSlotCount; i++) {
            pFrame->dpbImageResources[i] = m_pictures[pSlots->resourceIndices[i]];
        }
        pFrame->numDpbImageResources = pSlots->referenceSlotCount;
        pFrame->setupImageResource =
            (pSlots->pSetupReferenceSlot != nullptr) ? m_pictures[pSlots->resourceIndices[pSlots->referenceSlotCount]] : nullptr;
    }
    for (uint32_t index : m_releases) {
        m_pictures[index] = nullptr;
    }
    m_releases.clear();
}
