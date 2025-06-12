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

#ifndef _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_
#define _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_

#include <vector>
#include <atomic>
#include <cstdio>
#include <vulkan/vulkan.h>
#include "VkCodecUtils/VkVideoRefCountBase.h"

// Forward declarations
class VulkanDeviceContext;
class VulkanDecodedFrame;

/**
 * @brief Interface for writing video frames to a file.
 *
 * This class provides functionality to write decoded video frames to a file,
 * with support for various formats including Y4M and CRC generation.
 */
class VkVideoFrameOutput : public VkVideoRefCountBase {
public:
    /** @brief Reference to an invalid frame-to-file object used as default value */
    static VkSharedBaseObj<VkVideoFrameOutput>& invalidFrameToFile;

    /**
     * @brief Creates a new VkVideoFrameOutput instance
     *
     * @param fileName Output file name for the video frames
     * @param outputy4m Whether to output in Y4M format
     * @param outputcrcPerFrame Whether to generate CRC for each frame
     * @param crcOutputFile File name for CRC output
     * @param crcInitValue Initial CRC values
     * @param frameToFile Reference to store the created instance
     * @return VkResult VK_SUCCESS on success, error code otherwise
     */
    static VkResult Create(const char* fileName,
                          bool outputy4m = true,
                          bool outputcrcPerFrame = false,
                          const char* crcOutputFile = nullptr,
                          const std::vector<uint32_t>& crcInitValue = std::vector<uint32_t>(),
                          VkSharedBaseObj<VkVideoFrameOutput>& frameToFile = invalidFrameToFile);

    virtual ~VkVideoFrameOutput() = default;

    /**
     * @brief Outputs a decoded frame to file
     *
     * @param pFrame Pointer to the decoded frame
     * @param vkDevCtx Vulkan device context
     * @return size_t Number of bytes written, (size_t)-1 on error
     */
    virtual size_t OutputFrame(VulkanDecodedFrame* pFrame, const VulkanDeviceContext* vkDevCtx) = 0;

    /**
     * @brief Get the CRC values for the frame
     *
     * @param pCrcValues Pointer to store the CRC values
     * @param buffSize Size of the buffer to store the CRC values
     * @return size_t Number of CRC values written, (size_t)-1 on error
     */
    virtual size_t GetCrcValues(uint32_t* pCrcValues, size_t buffSize) const  = 0;
protected:
    VkVideoFrameOutput() = default;

private:
    // Prevent copying
    VkVideoFrameOutput(const VkVideoFrameOutput&) = delete;
    VkVideoFrameOutput& operator=(const VkVideoFrameOutput&) = delete;
};

#endif /* _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_ */
