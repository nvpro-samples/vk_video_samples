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

project(win32_semaphore_interop_test LANGUAGES CXX)

# Win32 handles are a Windows handle type.
if(NOT WIN32)
    message(STATUS "win32_semaphore_interop_test: Skipping (Windows-only)")
    return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The VkCodecUtils sources VulkanDeviceContext needs, as in the sibling win32_opaque_import test.
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

target_link_libraries(${PROJECT_NAME} PRIVATE ${VK_SHADER_COMPILER_LIBS})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
    VK_USE_PLATFORM_WIN32_KHR
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(VVS_REQUIRE_GPU)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VVS_REQUIRE_GPU=1)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Exit 0: every shared semaphore's signal arrived. Exit 1: one did not. Exit 77: the test could not
# run (no device, no Win32 semaphore export, no H.264 encode queue), a skip unless VVS_REQUIRE_GPU is
# set.
enable_testing()
add_test(NAME Win32SemaphoreInteropSignals
         COMMAND ${PROJECT_NAME})
set_tests_properties(Win32SemaphoreInteropSignals PROPERTIES
                     SKIP_RETURN_CODE 77
                     TIMEOUT 120
                     LABELS "gpu")

message(STATUS "win32_semaphore_interop_test: Configured for Windows")
