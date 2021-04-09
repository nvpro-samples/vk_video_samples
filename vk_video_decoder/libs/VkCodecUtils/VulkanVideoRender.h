/*
* Copyright 2020 NVIDIA Corporation.
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

#ifndef __VULKANVIDEORENDER__
#define __VULKANVIDEORENDER__

#include "PinnedBufferItem.h"

namespace vulkanVideoUtils {

class VkVideoAppCtx;

class VulkanVideoRender {

public:
    VulkanVideoRender(bool testVk = false)
    : pVkVideoAppCtx(nullptr),
      useTestImages(testVk)
    {

    }

    ~VulkanVideoRender()
    {
        Destroy();
    }

    // Initialize vulkan device context
    // after return, vulkan is ready to draw

    /* default imageFormat ex. VK_FORMAT_G8B8G8R8_422_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_G8B8G8R8_422_UNORM; */
    VkResult Init(VkFormat imageFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            int32_t videoWidth  = -1,
            int32_t videoHeight = -1,
            uint32_t format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
            android_dataspace dataSpace = (android_dataspace)(HAL_DATASPACE_STANDARD_BT709 |
                                                              HAL_DATASPACE_TRANSFER_SMPTE_170M |
                                                              HAL_DATASPACE_RANGE_FULL));

    VkResult CreateVertexBuffer();

    // delete vulkan device context when application goes away
    void Destroy();

    // Render a frame
    VkResult DrawFrame(android::sp<PinnedBufferItem>& inPinnedBufferItem,
                       android::sp<PinnedBufferItem>& outPinnedBufferItem);

    VkResult DrawTestFrame(int32_t inputBufferIndex = -1);

private:
    VkVideoAppCtx* pVkVideoAppCtx;
    bool useTestImages;
};

}

#endif // __VULKANVIDEORENDER__
