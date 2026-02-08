# Copyright 2024-2026 NVIDIA Corporation.
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

project(drm_format_mod_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only build on Linux/Unix (DRM modifiers are Linux-specific)
if(NOT UNIX OR APPLE)
    message(STATUS "drm_format_mod_test: Skipping (Linux-only)")
    return()
endif()

# Test application source files
set(TEST_SOURCES
    src/main.cpp
    src/DrmFormatModTest.cpp
    src/DrmFormats.cpp
)

# Test application header files
set(TEST_HEADERS
    inc/DrmFormatModTest.h
    inc/DrmFormats.h
)

# VkCodecUtils source files (from parent directory)
set(VKCODECUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../VkCodecUtils")
set(VKCODECUTILS_SOURCES
    ${VKCODECUTILS_DIR}/VulkanDeviceContext.cpp
    ${VKCODECUTILS_DIR}/VulkanDeviceMemoryImpl.cpp
    ${VKCODECUTILS_DIR}/VkImageResource.cpp
    ${VKCODECUTILS_DIR}/VkBufferResource.cpp
    ${VKCODECUTILS_DIR}/VulkanShaderCompiler.cpp
    ${VKCODECUTILS_DIR}/VulkanComputePipeline.cpp
    ${VKCODECUTILS_DIR}/VulkanDescriptorSetLayout.cpp
    ${VKCODECUTILS_DIR}/VulkanSamplerYcbcrConversion.cpp
    ${VKCODECUTILS_DIR}/VulkanCommandBufferPool.cpp
    ${VKCODECUTILS_DIR}/VulkanCommandBuffersSet.cpp
    ${VKCODECUTILS_DIR}/VulkanSemaphoreSet.cpp
    ${VKCODECUTILS_DIR}/VulkanFenceSet.cpp
    ${VKCODECUTILS_DIR}/Helpers.cpp
    ${VKCODECUTILS_DIR}/HelpersDispatchTable.cpp
    ${VKCODECUTILS_DIR}/nvVkFormats.cpp
)

# Create executable
add_executable(${PROJECT_NAME}
    ${TEST_SOURCES}
    ${TEST_HEADERS}
    ${VKCODECUTILS_SOURCES}
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../  # For VkCodecUtils headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include  # For vulkan_interfaces.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../..       # For nvidia_utils
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Find Vulkan with shaderc
find_package(Vulkan REQUIRED COMPONENTS shaderc_combined)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    Vulkan::shaderc_combined
)

# Platform-specific settings (Linux only at this point)
target_link_libraries(${PROJECT_NAME} PRIVATE
    pthread
    dl
)

# Ensure POSIX functions are available
target_compile_definitions(${PROJECT_NAME} PRIVATE _POSIX_C_SOURCE=200809L)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
)

# Installation
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add tests
enable_testing()
add_test(NAME DrmFormatModSmokeTest COMMAND ${PROJECT_NAME})
add_test(NAME DrmFormatModListFormats COMMAND ${PROJECT_NAME} --list-formats)
add_test(NAME DrmFormatModAllTests COMMAND ${PROJECT_NAME} --all)

# Print status
message(STATUS "drm_format_mod_test: Configured for Linux")
