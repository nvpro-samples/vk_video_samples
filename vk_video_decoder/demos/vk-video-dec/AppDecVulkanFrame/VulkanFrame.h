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

#ifndef SMOKE_H
#define SMOKE_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "NvVkDecoder/NvVkDecoder.h"
#include "VkShell/FrameProcessor.h"

#include "VulkanVideoProcessor.h"

class Meshes;
struct DecodedFrame;
class NvVkDecoder;

enum VIDEO_DECODER_PARSER_TYPE {
    DETECT_PARSER,
    H264_PARSER,
    H265_PARSER,
};

class VulkanFrame : public FrameProcessor {
public:
    VulkanFrame(const std::vector<std::string>& args);
    ~VulkanFrame();

    int attach_shell(Shell& sh);
    void detach_shell();

    int attach_swapchain();
    void detach_swapchain();

    virtual bool requires_vulkan_video() { return true; }

    void on_key(Key key);
    void on_tick();

    void on_frame(bool trainFrame = false);

    int GetVideoWidth();
    int GetVideoHeight();
    int init_internals(const VulkanDecodeContext vulkanDecodeContext);

private:
    enum { MAX_NUM_BUFFER_SLOTS = 16 };
    vulkanVideoUtils::VulkanDeviceInfo m_deviceInfo;
    uint32_t m_loopCount;

public:
    VkFormat frameImageFormat;
    VkSamplerYcbcrModelConversion samplerYcbcrModelConversion;
    VkSamplerYcbcrRange samplerYcbcrRange;
    vulkanVideoUtils::VkVideoAppCtx* pVideoRenderer;
    uint64_t lastRealTimeNsecs;

    struct Camera {
        glm::vec3 eye_pos;
        glm::mat4 view_projection;

        Camera(float eye)
            : eye_pos(eye)
        {
        }
    };

    struct FrameData {
        // signaled when this struct is ready for reuse
        DecodedFrame lastDecodedFrame;
    };

    // called by the constructor
    void init_workers();

    bool multithread_;
    bool use_push_constants_;

    // called mostly by on_key
    void update_camera();

    bool codec_paused_;
    Camera camera_;

    void create_frame_data(int count);
    void destroy_frame_data();

    VkQueue queue_;
    uint32_t queue_family_;
    VkFormat format_;

    VkPhysicalDeviceProperties physical_dev_props_;
    std::vector<VkMemoryPropertyFlags> mem_flags_;

    std::vector<FrameData> frame_data_;
    int frame_data_index_;

    VkClearValue render_pass_clear_value_;

    // called by attach_swapchain
    void prepare_viewport(const VkExtent2D& extent);

    VkExtent2D extent_;
    VkViewport viewport_;
    VkRect2D scissor_;

private:
    // Decoder specific members
    VulkanVideoProcessor m_videoProcessor;
    VIDEO_DECODER_PARSER_TYPE m_forceParserType;
};

#endif // HOLOGRAM_H
