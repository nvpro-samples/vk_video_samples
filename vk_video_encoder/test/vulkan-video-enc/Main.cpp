/*
 * Copyright 2024 NVIDIA Corporation.
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

#include <iostream>
#include "vulkan_video_encoder.h"

int main(int argc, const char** argv)
{
    std::cout << "Enter encoder test" << std::endl;
    VkSharedBaseObj<VulkanVideoEncoder> vulkanVideoEncoder;
    VkResult result = CreateVulkanVideoEncoder(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
                                  argc, argv, vulkanVideoEncoder);

    if (result != VK_SUCCESS) {
        std::cerr << "Error creating the encoder instance: " << result << std::endl;
    }

    int64_t numFrames = vulkanVideoEncoder->GetNumberOfFrames();
    std::cout << "Number of frames to encode: " << numFrames << std::endl;

    for (int64_t frameNum = 0; frameNum < numFrames; frameNum++) {
        int64_t frameNumEncoded = -1;
        result = vulkanVideoEncoder->EncodeNextFrame(frameNumEncoded);
        if (result != VK_SUCCESS) {
            std::cerr << "Error encoding frame: "  << frameNum  << ", error: " << result << std::endl;
        }
    }

    result = vulkanVideoEncoder->GetBitstream();
    if (result != VK_SUCCESS) {
        std::cerr << "Error obtaining the encoded bitstream file: " << result << std::endl;
    }

    std::cout << "Exit encoder test" << std::endl;
}


