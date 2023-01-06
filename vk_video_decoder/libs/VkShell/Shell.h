/*
 * Copyright (C) 2016 Google, Inc.
 * Copyright 2020 NVIDIA Corporation.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHELL_H
#define SHELL_H

#include <queue>
#include <vector>
#include <stdexcept>
#include <vulkan_interfaces.h>

#include "FrameProcessor.h"

static VkSemaphore vkNullSemaphore = VkSemaphore(0);

class FrameProcessor;

class Shell {
   public:
    Shell(const Shell &sh) = delete;
    Shell &operator=(const Shell &sh) = delete;
    virtual ~Shell() {}

    typedef enum BACK_BUFFER_STATE { BACK_BUFFER_INIT, BACK_BUFFER_PREPARE, BACK_BUFFER_IN_SWAPCHAIN, BACK_BUFFER_CANCELED } BACK_BUFFER_STATE;

    struct AcquireBuffer {

        AcquireBuffer();
        ~AcquireBuffer();
        VkResult Create(VkDevice dev);

        VkSemaphore semaphore_;
        VkFence fence_;

        VkDevice dev_;
    };

    class BackBuffer {

    public:
        BackBuffer();
        ~BackBuffer();
        VkResult Create(VkDevice dev);


        AcquireBuffer* SetAcquireBuffer(uint32_t imageIndex, AcquireBuffer* acquireBuffer) {
            AcquireBuffer* oldAcquireBuffer = acquireBuffer_;
            imageIndex_ = imageIndex;
            acquireBuffer_ = acquireBuffer;
            state_ = BACK_BUFFER_PREPARE;
            return oldAcquireBuffer;
        }

        const VkSemaphore& GetAcquireSemaphore() const {
            if (acquireBuffer_){
                return acquireBuffer_->semaphore_;
            } else {
                return vkNullSemaphore;
            }
        }

        const VkSemaphore& GetRenderSemaphore() const {
            return renderSemaphore_;
        }

        uint32_t GetImageIndex() const {
            return imageIndex_;
        }

        bool isInPrepareState() const {
            return ((state_ == BACK_BUFFER_PREPARE) && acquireBuffer_ != nullptr );
        }

        bool setBufferInSwapchain() {
            state_ = BACK_BUFFER_IN_SWAPCHAIN;
            return true;
        }

        bool setBufferCanceled() {
            state_ = BACK_BUFFER_CANCELED;
            return true;
        }

    private:
        uint32_t imageIndex_;

        AcquireBuffer* acquireBuffer_;
        VkSemaphore renderSemaphore_;

        BACK_BUFFER_STATE state_;
        VkDevice dev_;

    };

    struct Context {

        VkInstance instance;
        VkDebugReportCallbackEXT debug_report;

        VkPhysicalDevice physical_dev;
        uint32_t frameProcessor_queue_family;
        uint32_t present_queue_family;
        uint32_t video_decode_queue_family;
        uint32_t video_decode_queue_count;
        uint32_t video_encode_queue_family;
        bool     queryResultStatusSupport;

        VkDevice dev;
        VkQueue frameProcessor_queue;
        VkQueue present_queue;
        std::vector<VkQueue> video_queue;

        std::queue<AcquireBuffer*> acquireBuffers_;
        std::vector<BackBuffer> backBuffers_;
        uint32_t  currentBackBuffer_;

        VkDisplayKHR display_;
        uint32_t display_res_width_;
        uint32_t display_res_height_;

        VkSurfaceKHR surface;
        VkSurfaceFormatKHR format;

        VkSwapchainKHR swapchain;
        VkExtent2D extent;

        uint64_t acquiredFrameId;
    };
    const Context &context() const { return ctx_; }

    BackBuffer& GetCurrentBackBuffer() {
        return ctx_.backBuffers_[ctx_.currentBackBuffer_];
    }

    enum LogPriority {
        LOG_DEBUG,
        LOG_INFO,
        LOG_WARN,
        LOG_ERR,
    };
    virtual void log(LogPriority priority, const char *msg);

    virtual void run() = 0;
    virtual void quit() = 0;

   protected:
    Shell(FrameProcessor &frameProcessor);

    void init_vk(uint32_t deviceID);
    void cleanup_vk();

    void create_context();
    void destroy_context();

    void resize_swapchain(uint32_t width_hint, uint32_t height_hint);

    void add_frameProcessor_time(float time);

    void acquire_back_buffer(bool trainFrame = false);
    void present_back_buffer(bool trainFrame = false);

    FrameProcessor &frameProcessor_;
    const FrameProcessor::Settings &settings_;

    std::vector<const char *> instance_layers_;
    std::vector<const char *> instance_extensions_;

    std::vector<const char *> device_extensions_;

   private:
    bool debug_report_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type, uint64_t object, size_t location,
                               int32_t msg_code, const char *layer_prefix, const char *msg);
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type,
                                                                uint64_t object, size_t location, int32_t msg_code,
                                                                const char *layer_prefix, const char *msg, void *user_data) {
        Shell *shell = reinterpret_cast<Shell *>(user_data);
        return shell->debug_report_callback(flags, obj_type, object, location, msg_code, layer_prefix, msg);
    }

    void assert_all_instance_layers() const;
    void assert_all_instance_extensions() const;

    bool has_all_device_layers(VkPhysicalDevice phy) const;
    bool has_all_device_extensions(VkPhysicalDevice phy);

    // called by init_vk
    virtual PFN_vkGetInstanceProcAddr load_vk() = 0;
    virtual bool can_present(VkPhysicalDevice phy, uint32_t queue_family) = 0;
    void init_instance();
    void init_debug_report();
    void init_physical_dev(uint32_t deviceID);

    // called by create_context
    void create_dev();
    void create_back_buffers();
    void destroy_back_buffers();
    virtual VkSurfaceKHR create_surface(VkInstance instance) = 0;
    void create_swapchain();
    void destroy_swapchain();

   protected:
    void fake_present();
    Context ctx_;
  private:
    const float frameProcessor_tick_;
    float frameProcessor_time_;
};

#endif  // SHELL_H
