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

project(vk_filter_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test application source files
set(TEST_SOURCES
    src/main.cpp
    src/FilterTestApp.cpp
    src/TestCases.cpp
    src/ColorConversion.cpp
)

# Test application header files
set(TEST_HEADERS
    inc/FilterTestApp.h
    inc/TestCases.h
    inc/ColorConversion.h
)

# VkCodecUtils source files (from parent directory)
set(VKCODECUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../VkCodecUtils")
set(VKCODECUTILS_SOURCES
    ${VKCODECUTILS_DIR}/VulkanDeviceContext.cpp
    ${VKCODECUTILS_DIR}/VkEncoderStdioLatch.cpp
    ${VKCODECUTILS_DIR}/VulkanDeviceMemoryImpl.cpp
    ${VKCODECUTILS_DIR}/VkImageResource.cpp
    ${VKCODECUTILS_DIR}/VkBufferResource.cpp
    ${VKCODECUTILS_DIR}/VulkanFilterYuvCompute.cpp
    ${VKCODECUTILS_DIR}/VulkanFilter.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../  # For VkCodecUtils headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include  # For vulkan_interfaces.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..       # For nvidia_utils
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Vulkan is loaded at runtime (VK_NO_PROTOTYPES), so we only need headers.
# Use Vulkan SDK if available, otherwise fall back to project-fetched headers.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND TARGET Vulkan::Vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
elseif(TARGET vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${VK_SHADER_COMPILER_LIBS})

# Platform-specific settings
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pthread
        dl
    )
    # Ensure unistd.h functions are available
    target_compile_definitions(${PROJECT_NAME} PRIVATE _POSIX_C_SOURCE=200809L)
endif()

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

# Add test
enable_testing()
add_test(NAME FilterSmokeTest COMMAND ${PROJECT_NAME} --smoke)
add_test(NAME FilterAllTests COMMAND ${PROJECT_NAME} --all)
# Both of the above dispatch compute shaders and therefore need a real device.
# main() now exits 77 when Vulkan init fails for a device-availability reason,
# so a GPU-less host reports SKIPPED rather than FAILED. Every other init
# failure still exits 1, so a genuine regression cannot hide in the skip arm.
set_tests_properties(FilterSmokeTest FilterAllTests PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu")

# --list touches no Vulkan entry point at all: it builds the standard test
# table and prints it, and returns before app.init(). That makes it the one
# arm of this binary that belongs in the gating device-free set -- it proves
# the test-case table still constructs on every push.
add_test(NAME FilterListTests COMMAND ${PROJECT_NAME} --list)
set_tests_properties(FilterListTests PROPERTIES
                     LABELS "device-free")

# Add DRM Format Modifier test (Linux only)
if(UNIX AND NOT APPLE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/drm_format_mod")
        add_subdirectory(drm_format_mod)
    endif()
endif()

# Add Linux dma-buf SECOND-DEVICE import test (Linux only).
# The counterpart of win32_opaque_import: it proves an image exported by one
# VkDevice can be imported AND USED by a different VkDevice on the same GPU.
if(UNIX AND NOT APPLE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linux_dmabuf_import")
        add_subdirectory(linux_dmabuf_import)
    endif()
endif()

# Add Win32 Opaque Import test (Windows only)
if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win32_opaque_import")
        add_subdirectory(win32_opaque_import)
    endif()
endif()
