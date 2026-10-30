# Copyright 2026 NVIDIA Corporation.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)
project(descriptor_layout_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VulkanDescriptorSetLayoutInfo is defined in its header and calls no Vulkan
# entry point, so this test compiles the header on its own and links no
# VkCodecUtils source and no Vulkan library. The header reaches the generated
# dispatch table through VulkanDeviceContext.h, so the generator runs first.
add_executable(${PROJECT_NAME} src/main.cpp)
add_dependencies(${PROJECT_NAME} GenerateDispatchTables)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# WHAT THIS GATES. VulkanDescriptorSetLayoutInfo, the create info behind the
# compute filter's and the display's descriptor set layouts: every immutable
# sampler of every sampler binding is copied, each binding points at its own
# copies after the caller's arrays change or go away, a layout set again from
# its own create info keeps its samplers, a copy of a layout owns its bindings
# and samplers, and a pointer Vulkan ignores (a binding of another type, or
# with no descriptors) is neither read nor kept.
#
# WHAT A GREEN HERE DOES NOT MEAN. No layout is created and no descriptor is
# written, so it says nothing about what a device accepts.
#
# 0 means every expectation held, 1 means one missed.
enable_testing()
add_test(NAME DescriptorLayoutImmutableSamplers
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(DescriptorLayoutImmutableSamplers PROPERTIES
                     LABELS "device-free")

message(STATUS "descriptor_layout_test: Configured")
