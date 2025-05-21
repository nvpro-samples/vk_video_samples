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

cmake_minimum_required(VERSION 3.20.0)
project(${VULKAN_VIDEO_PARSER_LIB}, LANGUAGES CXX)

set(LIBNVPARSER
  include/VulkanH264Decoder.h
  include/VulkanH265Decoder.h
  include/VulkanH26xDecoder.h
  include/VulkanAV1Decoder.h
  include/VulkanVP9Decoder.h
  include/VulkanVideoDecoder.h
  ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkVideoRefCountBase.h
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
  src/cpudetect.cpp
)

include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/../")
include_directories(BEFORE ${VULKAN_VIDEO_PARSER_INCLUDE}/../)
include_directories(BEFORE ${VULKAN_VIDEO_PARSER_INCLUDE})
include_directories(BEFORE ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT})

add_library(${VULKAN_VIDEO_PARSER_LIB} SHARED ${LIBNVPARSER})

if ((CMAKE_SYSTEM_PROCESSOR MATCHES "^aarch64") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^arm64") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^ARM64"))
  MESSAGE(STATUS "Parser optimization for ARM64 ${CMAKE_SYSTEM_PROCESSOR}")
  if(WIN32)
    MESSAGE(STATUS "Parser optimizations selected for WIN32 armv8.0 and armv8.0+sve")
    set(NEON_CPU_FEATURE "/arch:armv8.0")
    # set(SVE_CPU_FEATURE "/arch:armv8.0+sve") # clang-cl limitation (SVE intrinsics are not supported by MSVC at the moment)
  elseif(UNIX)
    MESSAGE(STATUS "Parser optimizations selected for UNIX armv8-a and armv8-a+sve")
    set(NEON_CPU_FEATURE "-march=armv8-a")
    set(SVE_CPU_FEATURE "-march=armv8-a+sve")
  endif()
  MESSAGE(STATUS "Parser optimizations selected for generic ARM NEON")
  add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
  target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_neon OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeNEON.cpp include)
  set_target_properties(next_start_code_neon PROPERTIES COMPILE_FLAGS ${NEON_CPU_FEATURE} )
  target_include_directories(next_start_code_neon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  if(WIN32) # clang-cl limitation (SVE intrinsics are not supported by MSVC at the moment)
    MESSAGE(STATUS "Parser optimizations linking ARM64 next_start_code_c next_start_code_neon")
    target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_neon)
  elseif(UNIX)
    add_library(next_start_code_sve OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeSVE.cpp include)
    set_target_properties(next_start_code_sve PROPERTIES COMPILE_FLAGS ${SVE_CPU_FEATURE} )
    target_include_directories(next_start_code_sve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    MESSAGE(STATUS "Parser optimizations linking ARM64 next_start_code_c, next_start_code_neon, and next_start_code_sve")
    target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_neon next_start_code_sve)
  endif()
elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "^arm") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^ARM"))
  MESSAGE(STATUS "Parser optimization for ARM ${CMAKE_SYSTEM_PROCESSOR}")
  if(WIN32)
    MESSAGE(STATUS "Parser optimizations selected for WIN32 VFPv4")
    set(NEON_CPU_FEATURE "/arch:VFPv4")
  elseif(UNIX)
    MESSAGE(STATUS "Parser optimizations selected for UNIX armv7-a+simd")
    set(NEON_CPU_FEATURE "-march=armv7-a+simd")
  endif()
  add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
  target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_neon OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeNEON.cpp include)
  set_target_properties(next_start_code_neon PROPERTIES COMPILE_FLAGS ${NEON_CPU_FEATURE} )
  target_include_directories(next_start_code_neon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  MESSAGE(STATUS "Parser optimizations linking ARM next_start_code_c next_start_code_neon")
  target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_neon)
else()
  MESSAGE(STATUS "Parser optimization for X86 ${CMAKE_SYSTEM_PROCESSOR}")
  if(WIN32)
    MESSAGE(STATUS "Parser optimizations selected for WIN32 SSE2 AVX2")
    set(SSSE3_CPU_FEATURE "/arch:SSE2")
    set(AVX2_CPU_FEATURE "/arch:AVX2")
    # set(AVX512_CPU_FEATURE "/arch:AVX512")
  elseif(UNIX)
    MESSAGE(STATUS "Parser optimizations selected for UNIX -mssse3 -mavx2 -mavx512f -mavx512bw")
    set(SSSE3_CPU_FEATURE "-mssse3")
    set(AVX2_CPU_FEATURE "-mavx2")
    set(AVX512_CPU_FEATURE "-mavx512f -mavx512bw")
  endif()
  add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
  target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_ssse3 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeSSSE3.cpp include)
  set_target_properties(next_start_code_ssse3 PROPERTIES COMPILE_FLAGS ${SSSE3_CPU_FEATURE} )
  target_include_directories(next_start_code_ssse3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_avx2 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeAVX2.cpp include)
  set_target_properties(next_start_code_avx2 PROPERTIES COMPILE_FLAGS ${AVX2_CPU_FEATURE} )
  target_include_directories(next_start_code_avx2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_avx512 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeAVX512.cpp include)
  MESSAGE(STATUS "Parser optimizations adding x86 NextStartCodeSSSE3.cpp NextStartCodeAVX2.cpp NextStartCodeAVX512.cpp")
  if(NOT WIN32)
    set_target_properties(next_start_code_avx512 PROPERTIES COMPILE_FLAGS ${AVX512_CPU_FEATURE} )
  endif()
  target_include_directories(next_start_code_avx512 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  MESSAGE(STATUS "Parser optimizations linking x86 next_start_code_c next_start_code_ssse3 next_start_code_avx2 next_start_code_avx512")
  target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_ssse3 next_start_code_avx2 next_start_code_avx512)
endif()

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
                RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
if(WIN32)
    install(TARGETS ${VULKAN_VIDEO_PARSER_LIB} ${VULKAN_VIDEO_PARSER_STATIC_LIB}
                    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
                    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
                    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
            )
endif()
