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

set(LIBVKVIDEOENCODER_SRC
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_encoder.cpp
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
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoGopStructure.cpp
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoGopStructure.h
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}/VkVideoEncoder/VkVideoEncoder.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/YCbCrConvUtilsCpu.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/YCbCrConvUtilsCpu.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/Helpers.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanShaderCompiler.cpp
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
)

include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/../")
include_directories(BEFORE ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs)
include_directories(BEFORE ${VULKAN_VIDEO_ENCODER_INCLUDE})
include_directories(BEFORE ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT})

link_directories(
    ${VULKAN_LIB_DIR}
    )

add_library(${VULKAN_VIDEO_ENCODER_LIB} SHARED ${LIBVKVIDEOENCODER_SRC})
# Link the libraries
target_link_libraries(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${SHADERC_SHARED_LIBRARY})
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_ENCODER_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB} ${SHADERC_LIB})

target_include_directories(${VULKAN_VIDEO_ENCODER_LIB} PUBLIC ${VULKAN_VIDEO_ENCODER_INCLUDE} ${VULKAN_VIDEO_ENCODER_INCLUDE}/../NvVideoParser PRIVATE include)
target_compile_definitions(${VULKAN_VIDEO_ENCODER_LIB}
    PRIVATE VK_VIDEO_ENCODER_IMPLEMENTATION
    PUBLIC VK_VIDEO_ENCODER_SHAREDLIB
)

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
target_link_libraries(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${SHADERC_SHARED_LIBRARY})
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_ENCODER_STATIC_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB} ${SHADERC_LIB})
target_include_directories(${VULKAN_VIDEO_ENCODER_STATIC_LIB} PUBLIC ${VULKAN_VIDEO_ENCODER_INCLUDE} ${VULKAN_VIDEO_ENCODER_INCLUDE}/../NvVideoParser PRIVATE include)

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
