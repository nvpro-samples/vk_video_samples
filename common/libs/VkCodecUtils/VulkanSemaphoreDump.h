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

#pragma once

#include <vulkan/vulkan.h>
#include <iostream>
#include <iomanip>

namespace VulkanSemaphoreDump {

/**
 * @brief Dumps the semaphore information from a VkSubmitInfo2KHR structure
 * 
 * @param submitInfo The VkSubmitInfo2KHR structure containing semaphore information
 * @param submissionName Optional name to identify the submission (e.g., "DECODE", "COMPUTE")
 * @param decodeOrder Optional decode order number or identifier (uint64_t)
 * @param displayOrder Optional display order number or identifier (uint64_t)
 */
inline void DumpSemaphoreInfo(
    const VkSubmitInfo2KHR& submitInfo, 
    const char* submissionName = nullptr,
    uint64_t decodeEncodeOrder = UINT64_MAX,
    uint64_t displayInputOrder = UINT64_MAX)
{

    std::cout << "----------------------------\n";

    if (submissionName) {
        std::cout << submissionName << " ";
    }
    
    std::cout << "TL Semaphore sync";

    if (decodeEncodeOrder != UINT64_MAX) {
        std::cout << " (decode / encode = " << decodeEncodeOrder;
        if (displayInputOrder != UINT64_MAX) {
            std::cout << ", display / input = " << displayInputOrder;
        }
        std::cout << ")";
    } else if (displayInputOrder != UINT64_MAX) {
        std::cout << " (display / input = " << displayInputOrder << ")";
    }
    
    std::cout << ":\n";

    // Dump wait semaphores
    for (uint32_t i = 0; i < submitInfo.waitSemaphoreInfoCount; i++) {
        const VkSemaphoreSubmitInfoKHR& semInfo = submitInfo.pWaitSemaphoreInfos[i];
        std::cout << "  Wait sem[" << i << "]: " << semInfo.semaphore 
                  << " value = " << semInfo.value
                  << " stage = 0x" << std::hex << semInfo.stageMask << std::dec;

        if (semInfo.deviceIndex > 0) {
            std::cout << " deviceIndex=" << semInfo.deviceIndex;
        }
        std::cout << std::endl;
    }
    
    // Dump signal semaphores
    for (uint32_t i = 0; i < submitInfo.signalSemaphoreInfoCount; i++) {
        const VkSemaphoreSubmitInfoKHR& semInfo = submitInfo.pSignalSemaphoreInfos[i];
        std::cout << "  Signal sem[" << i << "]: " << semInfo.semaphore 
                  << " value = " << semInfo.value
                  << " stage = 0x" << std::hex << semInfo.stageMask << std::dec;

        if (semInfo.deviceIndex > 0) {
            std::cout << " deviceIndex = " << semInfo.deviceIndex;
        }
        std::cout << std::endl;
    }

    std::cout << "----------------------------" << std::endl;
}


} // namespace VulkanSemaphoreDump
