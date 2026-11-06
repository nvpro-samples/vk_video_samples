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

#ifndef _VKVIDEOENCODER_VKENCODERDPBPICTURES_H_
#define _VKVIDEOENCODER_VKENCODERDPBPICTURES_H_

#include <vector>

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "vkvideo_dpb/vvs_dpb.hpp"

// The pictures of an encoder's DPB library context: images of the encoder's DPB image pool, named by
// their index in it, which the context takes and gives back through its resource callbacks. A frame
// holds the images its commands read and write until it completes.
class VkEncoderDpbPictures {
   public:
    // The callbacks for VvsDpbCreateInfo::resources, taking images from the pool *pPool holds when
    // the context calls them.
    VvsDpbResources Resources(const VkSharedBaseObj<VulkanVideoImagePool>* pPool);

    // Around a picture call: pFrame is the frame whose picture the call sets up; its picture is
    // reconstructed at the coded offset and extent of its input.
    void BeginPicture(VkVideoEncoder::VkVideoEncodeFrameInfo* pFrame) { m_pFrame = pFrame; }
    // After the call: on success the frame holds the images of the slots, setupImageResource and
    // dpbImageResources[] for the references; then the images the DPB released are dropped.
    void EndPicture(VkVideoEncoder::VkVideoEncodeFrameInfo* pFrame, const VvsDpbPictureSlots* pSlots);

   private:
    static VkResult VKAPI_CALL Acquire(void* pUserData, const VvsDpbPictureInfo* pPictureInfo, uint32_t* pResourceIndex,
                                       const VkVideoPictureResourceInfoKHR** ppResource);
    static void VKAPI_CALL Retain(void* pUserData, uint32_t resourceIndex);
    static void VKAPI_CALL Release(void* pUserData, uint32_t resourceIndex);

    const VkSharedBaseObj<VulkanVideoImagePool>* m_pPool = nullptr;
    // The images of the pictures the DPB holds, by index in the DPB image pool.
    std::vector<VkSharedBaseObj<VulkanVideoImagePoolNode>> m_pictures;
    // The images the DPB released during the picture call in progress.
    std::vector<uint32_t> m_releases;
    VkVideoEncoder::VkVideoEncodeFrameInfo* m_pFrame = nullptr;
};

#endif /* _VKVIDEOENCODER_VKENCODERDPBPICTURES_H_ */
