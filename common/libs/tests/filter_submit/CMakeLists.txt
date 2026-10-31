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
project(filter_submit_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VKCODECUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../VkCodecUtils")
add_executable(${PROJECT_NAME}
    src/main.cpp
    ${VKCODECUTILS_DIR}/VulkanDeviceContext.cpp
    ${VKCODECUTILS_DIR}/VkEncoderStdioLatch.cpp
    ${VKCODECUTILS_DIR}/VulkanFilter.cpp
    ${VKCODECUTILS_DIR}/VulkanShaderCompiler.cpp
    ${VKCODECUTILS_DIR}/${VK_SHADER_COMPILER_BACKEND_SOURCE}
    ${VKCODECUTILS_DIR}/VulkanCommandBufferPool.cpp
    ${VKCODECUTILS_DIR}/VulkanCommandBuffersSet.cpp
    ${VKCODECUTILS_DIR}/VulkanSemaphoreSet.cpp
    ${VKCODECUTILS_DIR}/VulkanFenceSet.cpp
    ${VKCODECUTILS_DIR}/VulkanQueryPoolSet.cpp
    ${VKCODECUTILS_DIR}/Helpers.cpp
    ${VKCODECUTILS_DIR}/nvVkFormats.cpp
    ${VK_DISPATCH_TABLE_SOURCE}
)
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

target_link_libraries(${PROJECT_NAME} PRIVATE ${VK_SHADER_COMPILER_LIBS})
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread dl)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# WHAT THIS GATES. A VulkanFilter submits to the queue it retrieves itself;
# VulkanDeviceContext::MultiThreadedQueueSubmit() names its queue by role. Two
# filters and a compute-role submitter on the compute queue, and a filter and a
# graphics-role submitter on the graphics queue, each on its own thread, submit
# at the same time with the validation layer's thread-safety check on, and the
# layer reports nothing: every submit to one VkQueue takes that queue's lock.
# On a device whose compute and graphics queues are one VkQueue, the five
# threads share it.
#
# WHAT A GREEN HERE DOES NOT MEAN. Host access to a queue outside these two
# paths (a present, a wait for idle) is not exercised; and a layer whose
# thread-safety check is off would report nothing either way. The test sets
# VK_KHRONOS_VALIDATION_THREAD_SAFETY=true for that reason.
#
# Not validation-gated: the gate counts VUID-named messages, and the
# thread-safety check's messages carry no VUID. The test counts every error
# the layer reports through its own messenger.
#
# 0 means no error, 1 an error or a submit failure, 77 no validation layer, or
# no device (a failure with VVS_REQUIRE_GPU=1; a missing layer stays a skip).
enable_testing()
add_test(NAME FilterSubmitThreads
         COMMAND ${PROJECT_NAME})
set_tests_properties(FilterSubmitThreads PROPERTIES
                     LABELS "gpu"
                     SKIP_RETURN_CODE 77
                     TIMEOUT 300)

message(STATUS "filter_submit_test: Configured")
