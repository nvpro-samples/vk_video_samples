# SPDX-FileCopyrightText: Copyright (c) 2021 - 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

cmake_minimum_required(VERSION 2.8.12)
project(${VULKAN_VIDEO_PARSER_LIB}, LANGUAGES CXX)

# Even though shaderc.hpp is a headers-only library, adding
# a dependency here will force clients of the library to rebuild
# when it changes.
set(LIBNVPARSER
  include/VulkanH264Decoder.h
  include/VulkanH265Decoder.h
  include/VulkanH26xDecoder.h
  include/VulkanVideoDecoder.h
  ../../../common/include/VkVideoCore/VkVideoRefCountBase.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/../NvVideoParser/nvVulkanVideoUtils.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParser.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParserParams.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/PictureBufferBase.h
  ${VULKAN_VIDEO_PARSER_INCLUDE}/VulkanVideoParserIf.h
  src/VulkanH264Parser.cpp
  src/VulkanH265Parser.cpp
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
