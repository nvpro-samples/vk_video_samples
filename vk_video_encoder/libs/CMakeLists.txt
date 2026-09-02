# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20.0)
project(${VULKAN_VIDEO_ENCODER_LIB}, LANGUAGES CXX)

# simdjson (Apache-2.0) for encoder config JSON loading
include(FetchContent)
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.6.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson)

# THE ENCODER'S OS SEAM, selected here and nowhere else. Two translation
# units carry everything the encoder needs from the host OS -- thread naming
# and DRM format modifier selection, and the waitable completion handle -- and
# each has one implementation per platform, chosen by this block. Every
# implementation begins with an #error guarding its own platform, so a unit
# selected for the wrong target fails at its first line rather than at a
# missing symbol during the link.
#
# An unsupported target is refused HERE, at configure time, with the port
# named: a platform with no seam cannot build the encoder, and saying so at
# configure is cheaper than a preprocessor error partway through.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(VK_VIDEO_ENCODER_OS_ADAPTER_SOURCE
        ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderOsAdapterLinux.cpp)
    set(VK_VIDEO_ENCODER_OS_EVENT_SOURCE
        ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder_os_event_linux.cpp)
elseif(WIN32)
    set(VK_VIDEO_ENCODER_OS_ADAPTER_SOURCE
        ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderOsAdapterWindows.cpp)
    set(VK_VIDEO_ENCODER_OS_EVENT_SOURCE
        ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder_os_event_windows.cpp)
else()
    message(FATAL_ERROR
        "The Vulkan video encoder library has no OS seam implementation for "
        "${CMAKE_SYSTEM_NAME}. Two translation units implement the seam; a "
        "port supplies a sibling of each and selects it in this file:\n"
        "    VkVideoEncoder/VkVideoEncoderOsAdapter<Os>.cpp -- thread naming "
        "and DRM format modifier selection\n"
        "    src/vulkan_video_encoder_os_event_<os>.cpp -- the waitable "
        "completion handle\n"
        "Configure with -DBUILD_ENCODER=OFF to build the rest of the tree "
        "without the encoder.")
endif()

set(LIBVKVIDEOENCODER_SRC
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder_ext.cpp
    ${VK_VIDEO_ENCODER_OS_EVENT_SOURCE}
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder_os_event_linux.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigH264.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigH264.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbH264.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbH264.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderH264.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderH264.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigH265.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigH265.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbH265.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbH265.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderH265.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderH265.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigAV1.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfigAV1.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbAV1.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderDpbAV1.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderAV1.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderAV1.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkEncoderConfig.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoder.cpp
    ${VK_VIDEO_ENCODER_OS_ADAPTER_SOURCE}
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderOsAdapterLinux.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoGopStructure.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoGopStructure.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoder.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderPsnr.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoderPsnr.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/json/EncoderConfigJsonLoader.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/YCbCrConvUtilsCpu.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/YCbCrConvUtilsCpu.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/Helpers.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/Helpers.h
    ${VK_DISPATCH_TABLE_SOURCE}
    ${VK_DISPATCH_TABLE_HEADER}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkThreadSafeQueue.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkThreadSafeQueue.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBufferPool.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBufferPool.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanEncoderInputFrame.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanEncoderInputFrame.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanShaderCompiler.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/${VK_SHADER_COMPILER_BACKEND_SOURCE}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanShaderCompiler.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceMemoryImpl.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceMemoryImpl.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkBufferResource.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkBufferResource.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkImageResource.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkImageResource.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoImagePool.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoImagePool.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBufferPool.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBufferPool.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanQueryPoolSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanQueryPoolSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDescriptorSetLayout.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDescriptorSetLayout.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBuffersSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBuffersSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSession.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSession.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSessionParameters.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSessionParameters.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/pattern.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/pattern.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanComputePipeline.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanComputePipeline.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilter.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilter.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilterYuvCompute.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilterYuvCompute.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSamplerYcbcrConversion.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSamplerYcbcrConversion.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFenceSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFenceSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSemaphoreSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSemaphoreSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkVideoRefCountBase.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/nvVkFormats.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBistreamBufferImpl.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBistreamBufferImpl.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkVideoCrc.cpp
)

# The internal ext header includes the public header by its tree-rooted
# path (third_party/vulkan_video_samples/src/...). The GN build resolves
# that via the Chromium source root on the include path; mirror it here
# with the directory five levels up (the parent of third_party/).
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/../")
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}")
include_directories(BEFORE ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs)
include_directories(BEFORE ${VULKAN_VIDEO_ENCODER_INCLUDE})
include_directories(BEFORE ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT})

link_directories(
    ${VULKAN_LIB_DIR}
    )

add_library(${VULKAN_VIDEO_ENCODER_LIB} SHARED ${LIBVKVIDEOENCODER_SRC})
# Link the libraries
target_link_libraries(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${VK_SHADER_COMPILER_LIBS} simdjson::simdjson)
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_ENCODER_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB})

# Link against nvenc_aq_vulkan if available
if(NV_AQ_GPU_LIB_AVAILABLE AND TARGET ${NV_AQ_GPU_LIB_TARGET})
    message(STATUS "Linking ${VULKAN_VIDEO_ENCODER_LIB} against ${NV_AQ_GPU_LIB_TARGET}")
    target_link_libraries(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${NV_AQ_GPU_LIB_TARGET})
    add_dependencies(${VULKAN_VIDEO_ENCODER_LIB} ${NV_AQ_GPU_LIB_TARGET})
endif()

target_include_directories(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${VULKAN_VIDEO_ENCODER_INCLUDE} ${VULKAN_VIDEO_ENCODER_INCLUDE}/../NvVideoParser PRIVATE include)
target_compile_definitions(${VULKAN_VIDEO_ENCODER_LIB}
    PRIVATE VK_VIDEO_ENCODER_IMPLEMENTATION
    PUBLIC VK_VIDEO_ENCODER_SHAREDLIB
)

# Add NV_AQ_GPU_LIB_SUPPORTED definition and include paths if the library is available
if(NV_AQ_GPU_LIB_AVAILABLE)
    target_compile_definitions(${VULKAN_VIDEO_ENCODER_LIB} PRIVATE NV_AQ_GPU_LIB_SUPPORTED=1)
    # Add all AQ include directories from the centralized list
    if(NV_AQ_GPU_LIB_INCLUDE_DIRS)
        target_include_directories(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${NV_AQ_GPU_LIB_INCLUDE_DIRS})
        message(STATUS "Added AQ include directories to ${VULKAN_VIDEO_ENCODER_LIB}")
    endif()
endif()

find_package(Threads)

set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES SOVERSION ${VULKAN_VIDEO_ENCODER_LIB_VERSION})

if(WIN32)
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES COMPILE_DEFINITIONS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_ENCODER_LIB} PROPERTIES LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS")
endif()

add_library(${VULKAN_VIDEO_ENCODER_STATIC_LIB} STATIC ${LIBVKVIDEOENCODER_SRC})
# Link the libraries
target_link_libraries(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${VK_SHADER_COMPILER_LIBS} simdjson::simdjson)
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_ENCODER_STATIC_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB})

# Link against nvenc_aq_vulkan if available
if(NV_AQ_GPU_LIB_AVAILABLE AND TARGET ${NV_AQ_GPU_LIB_TARGET})
    message(STATUS "Linking ${VULKAN_VIDEO_ENCODER_STATIC_LIB} against ${NV_AQ_GPU_LIB_TARGET}")
    target_link_libraries(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${NV_AQ_GPU_LIB_TARGET})
    add_dependencies(${VULKAN_VIDEO_ENCODER_STATIC_LIB} ${NV_AQ_GPU_LIB_TARGET})
endif()
target_include_directories(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${VULKAN_VIDEO_ENCODER_INCLUDE} ${VULKAN_VIDEO_ENCODER_INCLUDE}/../NvVideoParser PRIVATE include)

# Add NV_AQ_GPU_LIB_SUPPORTED definition and include paths if the library is available
if(NV_AQ_GPU_LIB_AVAILABLE)
    target_compile_definitions(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PRIVATE NV_AQ_GPU_LIB_SUPPORTED=1)
    # Add all AQ include directories from the centralized list
    if(NV_AQ_GPU_LIB_INCLUDE_DIRS)
        target_include_directories(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${NV_AQ_GPU_LIB_INCLUDE_DIRS})
        message(STATUS "Added AQ include directories to ${VULKAN_VIDEO_ENCODER_STATIC_LIB}")
    endif()
endif()

install(TARGETS ${VULKAN_VIDEO_ENCODER_LIB} ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
                RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

if(WIN32)
    install(TARGETS ${VULKAN_VIDEO_ENCODER_LIB} ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
                    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
                    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
                    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
            )
endif()
