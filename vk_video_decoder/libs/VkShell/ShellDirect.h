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

#ifndef SHELL_DIRECT_H
#define SHELL_DIRECT_H

#include "Shell.h"

// direct to display without Window server using  VK_KHR_display and VK_EXT_direct_mode_display
class ShellDirect : public Shell {
  public:
    ShellDirect(FrameProcessor& frameProcessor, uint32_t deviceID);
   ~ShellDirect();

    virtual void run() override;
    virtual void quit() override;

private:
    virtual PFN_vkGetInstanceProcAddr load_vk() override;
    virtual VkSurfaceKHR create_surface(VkInstance instance) override;
    virtual bool can_present(VkPhysicalDevice phy, uint32_t queue_family) override;
private:
    void init_display();

    void* lib_handle_;
    bool quit_;
};

#endif // SHELL_DIRECT_H
