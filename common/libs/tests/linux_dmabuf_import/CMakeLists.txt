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

project(linux_dmabuf_import_test LANGUAGES CXX)

# Only build on Linux/Unix (dma-buf is a Linux kernel object)
if(NOT UNIX OR APPLE)
    message(STATUS "linux_dmabuf_import_test: Skipping (Linux-only)")
    return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test application source files
set(TEST_SOURCES
    src/main.cpp
    src/LinuxDmaBufImportTest.cpp
)

# Test application header files
set(TEST_HEADERS
    inc/LinuxDmaBufImportTest.h
)

# VkCodecUtils source files (from parent directory) - same set the sibling
# drm_format_mod and win32_opaque_import tests link, so VulkanDeviceContext's
# dependencies are satisfied identically.
set(VKCODECUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../VkCodecUtils")
set(VKCODECUTILS_SOURCES
    ${VKCODECUTILS_DIR}/VulkanDeviceContext.cpp
    ${VKCODECUTILS_DIR}/VkEncoderStdioLatch.cpp
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

# Create executable
add_executable(${PROJECT_NAME}
    ${TEST_SOURCES}
    ${TEST_HEADERS}
    ${VKCODECUTILS_SOURCES}
)

# The dispatch table is generated into the build tree, not checked in.
# Listing the generated source gives THIS .cpp a file-level dependency,
# but every other TU here reaches the generated HEADER through
# inc/LinuxDmaBufImportTest.h -> VkCodecUtils/VulkanDeviceContext.h, and
# nothing orders those objects after the generator without this.
add_dependencies(${PROJECT_NAME} GenerateDispatchTables)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../  # For VkCodecUtils headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include  # For vulkan_interfaces.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../..       # For nvidia_utils
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Vulkan is loaded at runtime (VK_NO_PROTOTYPES), so we only need headers.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND TARGET Vulkan::Vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
elseif(TARGET vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${VK_SHADER_COMPILER_LIBS})

# Platform-specific settings (Linux only at this point)
target_link_libraries(${PROJECT_NAME} PRIVATE
    pthread
    dl
)

# Ensure POSIX functions are available (dup/close)
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

# Add tests.
#
# NOTE ON CTest SEMANTICS: this executable returns 0 only when the
# second-device import AND the content round-trip both succeeded. Exit code 2
# means "could not run" (no GPU, missing extension, nothing proved) and is
# deliberately a CTest FAILURE, not a skip - a host that cannot run this test
# must not report the device-independence claim as verified.
enable_testing()
add_test(NAME LinuxDmaBufSecondDeviceImport
         COMMAND ${PROJECT_NAME})
add_test(NAME LinuxDmaBufSecondDeviceImportLinear
         COMMAND ${PROJECT_NAME} --linear-only --verbose)
add_test(NAME LinuxDmaBufSecondDeviceImportRGBA
         COMMAND ${PROJECT_NAME} --format rgba8 --no-video-usage)

# Exit 2 ("COULD NOT RUN - THIS IS NOT A PASS") is mapped to CTest SKIP rather
# than to FAIL. That does not weaken the note above: a CTest SKIP is not a
# pass, so the device-independence claim is still not reported as verified on
# a host that cannot run this. It only stops a GPU-less CI runner from being
# permanently red, which is the state that teaches people to ignore the job.
# Exit 1 -- the import ran and the round-trip did not hold -- remains a FAIL.
set_tests_properties(LinuxDmaBufSecondDeviceImport
                     LinuxDmaBufSecondDeviceImportLinear
                     LinuxDmaBufSecondDeviceImportRGBA PROPERTIES
                     SKIP_RETURN_CODE 2
                     LABELS "gpu")

# Print status
message(STATUS "linux_dmabuf_import_test: Configured for Linux")
