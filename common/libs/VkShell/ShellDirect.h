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
    ShellDirect(const VulkanDeviceContext* vkDevCtx, const Configuration& configuration);
    virtual ~ShellDirect();

    static const char* GetRequiredInstanceExtension();
    static const std::vector<VkExtensionProperties>& GetRequiredInstanceExtensions();
    virtual bool PhysDeviceCanPresent(VkPhysicalDevice physicalDevice, uint32_t presentQueueFamily) const override;
    virtual void RunLoop() override;
    virtual void QuitLoop() override;

private:
    virtual VkSurfaceKHR CreateSurface(VkInstance instance) override;
private:
    void InitDisplay();

    VkDisplayKHR m_vkDisplay;
    uint32_t     m_displayWidth;
    uint32_t     m_displayHeight;
    bool         m_quitLoop;
};

#endif // SHELL_DIRECT_H
