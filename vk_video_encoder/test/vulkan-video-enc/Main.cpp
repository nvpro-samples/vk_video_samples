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

#include <cstdlib>
#include <iostream>
#include "vulkan_video_encoder_argv.h"
#include "VkVSCommon.h"

int main(int argc, const char** argv)
{
    std::cout << "Enter encoder test" << std::endl;
    VkSharedBaseObj<VulkanVideoEncoder> vulkanVideoEncoder;
    VkResult result = CreateVulkanVideoEncoder(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
                                  argc, argv, vulkanVideoEncoder);

    if (result != VK_SUCCESS) {
        std::cerr << "Error creating the encoder instance: " << result << std::endl;
        // 69 is reserved for a device that cannot do this, and for nothing
        // else. Every other creation failure -- out of memory, initialization,
        // device loss, unknown -- is an ordinary failure, because a harness
        // that reads 69 as "skip" would otherwise skip a real defect.
        return IsVideoUnsupportedResult(result) ? VVS_EXIT_UNSUPPORTED : EXIT_FAILURE;
    }

    // A VK_SUCCESS that hands back nothing usable is still a failure, and one
    // that would be invisible below: the frame loop simply would not run and
    // the process would exit zero having encoded nothing.
    if (!vulkanVideoEncoder) {
        std::cerr << "Error: encoder creation reported success but produced no "
                     "encoder" << std::endl;
        return EXIT_FAILURE;
    }

    const int64_t numFrames = vulkanVideoEncoder->GetNumberOfFrames();
    if (numFrames < 0) {
        std::cerr << "Error: invalid frame count " << numFrames << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Number of frames to encode: " << numFrames << std::endl;

    // THE FIRST ERROR IS KEPT. Nothing below may overwrite it -- not a later
    // frame that happens to succeed, and not a completion that succeeds.
    VkResult firstError = VK_SUCCESS;

    for (int64_t frameNum = 0; frameNum < numFrames; frameNum++) {
        int64_t frameNumEncoded = -1;
        result = vulkanVideoEncoder->EncodeNextFrame(frameNumEncoded);
        if (result != VK_SUCCESS) {
            std::cerr << "Error encoding frame: "  << frameNum  << ", error: " << result << std::endl;
            firstError = result;
            // Stop asking. Continuing past a failed frame produces a stream
            // with a hole in it and a longer log that says the same thing.
            break;
        }
    }

    // Called once for an initialized session even after a frame failed: the
    // work already accepted has to be completed and the output closed.
    result = vulkanVideoEncoder->GetBitstream();
    if (result != VK_SUCCESS) {
        std::cerr << "Error obtaining the encoded bitstream file: " << result << std::endl;
        if (firstError == VK_SUCCESS) {
            firstError = result;
        }
    }

    std::cout << "Exit encoder test" << std::endl;
    // EXPLICIT. Falling off the end of main returns zero, which is how a run
    // that printed a failure for every frame was still read as a pass. A
    // post-initialization failure is EXIT_FAILURE even when its VkResult looks
    // like a capability answer -- the device was already known to be capable,
    // or creation would have returned 69.
    return (firstError == VK_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
