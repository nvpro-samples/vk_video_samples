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

project(linux_sync_fd_export_test LANGUAGES CXX)

# SYNC_FD is a Linux handle type.
if(NOT UNIX OR APPLE)
    message(STATUS "linux_sync_fd_export_test: Skipping (Linux-only)")
    return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The VkCodecUtils sources VulkanDeviceContext needs, as in the sibling linux_dmabuf_import test.
set(VKCODECUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../VkCodecUtils")
set(VKCODECUTILS_SOURCES
    ${VKCODECUTILS_DIR}/VulkanDeviceContext.cpp
    ${VKCODECUTILS_DIR}/VulkanDeviceMemoryImpl.cpp
    ${VKCODECUTILS_DIR}/VkImageResource.cpp
    ${VKCODECUTILS_DIR}/VkBufferResource.cpp
    ${VKCODECUTILS_DIR}/VulkanShaderCompiler.cpp
    ${VKCODECUTILS_DIR}/${VK_SHADER_COMPILER_BACKEND_SOURCE}
    ${VKCODECUTILS_DIR}/VulkanComputePipeline.cpp
    ${VKCODECUTILS_DIR}/VulkanDescriptorSetLayout.cpp
    ${VKCODECUTILS_DIR}/VulkanSamplerYcbcrConversion.cpp
    ${VKCODECUTILS_DIR}/VulkanCommandBufferPool.cpp
    ${VKCODECUTILS_DIR}/VulkanCommandBuffersSet.cpp
    ${VKCODECUTILS_DIR}/VulkanSemaphoreSet.cpp
    ${VKCODECUTILS_DIR}/VulkanFenceSet.cpp
    ${VKCODECUTILS_DIR}/Helpers.cpp
    ${VK_DISPATCH_TABLE_SOURCE}
    ${VK_DISPATCH_TABLE_HEADER}
    ${VKCODECUTILS_DIR}/nvVkFormats.cpp
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    ${VKCODECUTILS_SOURCES}
)

# The dispatch table is generated into the build tree; VulkanDeviceContext.h includes its header.
add_dependencies(${PROJECT_NAME} GenerateDispatchTables)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../           # VkCodecUtils headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include # vulkan_interfaces.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../..      # nvidia_utils
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Vulkan is loaded at run time (VK_NO_PROTOTYPES); only the headers are needed to build.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND TARGET Vulkan::Vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
elseif(TARGET vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${VK_SHADER_COMPILER_LIBS} pthread dl)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _POSIX_C_SOURCE=200809L
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
)
if(VVS_REQUIRE_GPU)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VVS_REQUIRE_GPU=1)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Exit 0: every exported SYNC_FD signalled. Exit 1: one did not. Exit 77: the test could not run (no
# device, no SYNC_FD export, no H.264 encode queue), a skip unless VVS_REQUIRE_GPU is set.
enable_testing()
add_test(NAME LinuxSyncFdExportSignals
         COMMAND ${PROJECT_NAME})
set_tests_properties(LinuxSyncFdExportSignals PROPERTIES
                     SKIP_RETURN_CODE 77
                     TIMEOUT 120
                     LABELS "gpu")

message(STATUS "linux_sync_fd_export_test: Configured for Linux")
