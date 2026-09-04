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

#include "VkCodecUtils/VkEncoderStdioLatch.h"

//=============================================================================
// The one definition of the stdio-silence counter.
//
// IT LIVES ALONE, and that is the point. It used to sit in
// VulkanDeviceContext.cpp, which meant every target compiling any file that
// routes output through the gate had to compile the device context too. The
// standalone decoder and demo targets pick individual VkCodecUtils sources
// and do not, so they failed to link the moment those sources started using
// the gate.
//
// This unit includes the latch header and nothing else -- no Vulkan, no
// device, no library link closure -- so a target can add it without taking on
// anything it was deliberately avoiding.
//
// Still exactly ONE definition, for the reason the header spells out: a
// function-local static behind an internal-linkage accessor would give every
// translation unit its own counter, and a silence request made in one would
// be invisible to the rest.
//=============================================================================
std::atomic<int>& VkEncoderStdioSilenceCountRef()
{
    static std::atomic<int> activeSilenceRequests{0};
    return activeSilenceRequests;
}
