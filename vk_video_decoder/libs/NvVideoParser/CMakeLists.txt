# SPDX-FileCopyrightText: Copyright (c) 2021 - 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

cmake_minimum_required(VERSION 2.8.12)
project(${VULKAN_VIDEO_PARSER_LIB}, LANGUAGES CXX)

# Even though shaderc.hpp is a headers-only library, adding
# a dependency here will force clients of the library to rebuild
# when it changes.
set(LIBNVPARSER
  include/VulkanH264Decoder.h
  include/VulkanH265Decoder.h
  include/VulkanH26xDecoder.h
  include/VulkanAV1Decoder.h
  include/VulkanVP9Decoder.h
  include/VulkanVideoDecoder.h
  ../../../common/include/VkVideoCore/VkVideoRefCountBase.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/../NvVideoParser/nvVulkanVideoUtils.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParser.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParserParams.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/PictureBufferBase.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParserIf.h
  src/VulkanH264Parser.cpp
  src/VulkanH265Parser.cpp
  src/VulkanAV1Decoder.cpp
  src/VulkanVP9Decoder.cpp
  src/VulkanAV1GlobalMotionDec.cpp
  src/VulkanVideoDecoder.cpp
  src/nvVulkanh264ScalingList.cpp
)

include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/../")
include_directories(BEFORE ${VULKAN_VIDEO_PARSER_INCLUDE})

add_library(${VULKAN_VIDEO_PARSER_LIB} SHARED ${LIBNVPARSER})

target_include_directories(${VULKAN_VIDEO_PARSER_LIB} PUBLIC ${VULKAN_VIDEO_PARSER_INCLUDE} ${VULKAN_VIDEO_PARSER_INCLUDE}/../NvVideoParser PRIVATE include)
target_compile_definitions(${VULKAN_VIDEO_PARSER_LIB}
    PRIVATE NVPARSER_IMPLEMENTATION
    PUBLIC NVPARSER_SHAREDLIB
)

find_package(Threads)

set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES SOVERSION ${VULKAN_VIDEO_PARSER_LIB_VERSION})

if(WIN32)
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES COMPILE_DEFINITIONS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_PARSER_LIB} PROPERTIES LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS")
endif()

add_library(${VULKAN_VIDEO_PARSER_STATIC_LIB} STATIC ${LIBNVPARSER})
target_include_directories(${VULKAN_VIDEO_PARSER_STATIC_LIB} PUBLIC ${VULKAN_VIDEO_PARSER_INCLUDE} ${VULKAN_VIDEO_PARSER_INCLUDE}/../NvVideoParser PRIVATE include)

install(TARGETS ${VULKAN_VIDEO_PARSER_LIB} ${VULKAN_VIDEO_PARSER_STATIC_LIB}
                RUNTIME DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
                ARCHIVE DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
                LIBRARY DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
        )

install(TARGETS ${VULKAN_VIDEO_PARSER_LIB} ${VULKAN_VIDEO_PARSER_STATIC_LIB}
                RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
