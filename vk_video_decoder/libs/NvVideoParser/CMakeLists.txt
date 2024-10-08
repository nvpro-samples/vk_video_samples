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

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^aarch64")
  if(WIN32)
    set(GENERIC_CPU_FEATURE "/arch:armv8.0+nosimd")
    set(NEON_CPU_FEATURE "/arch:armv8.0")
    set(SVE_CPU_FEATURE "/arch:armv8.2+sve")
  elseif(UNIX)
    set(GENERIC_CPU_FEATURE "-march=armv8-a+nosimd")
    set(NEON_CPU_FEATURE "-march=armv8-a")
    set(SVE_CPU_FEATURE "-march=armv8.2-a+sve")
  endif()
    add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
    set_target_properties(next_start_code_c PROPERTIES COMPILE_FLAGS ${GENERIC_CPU_FEATURE} )
    target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_library(next_start_code_neon OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeNEON.cpp include)
    set_target_properties(next_start_code_neon PROPERTIES COMPILE_FLAGS ${NEON_CPU_FEATURE} )
    target_include_directories(next_start_code_neon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_library(next_start_code_sve OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeSVE.cpp include)
    set_target_properties(next_start_code_sve PROPERTIES COMPILE_FLAGS ${SVE_CPU_FEATURE} )
    target_include_directories(next_start_code_sve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_neon next_start_code_sve)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^aarch32")
  if(WIN32)
    set(GENERIC_CPU_FEATURE "/arch:ARMv7VE")
    set(NEON_CPU_FEATURE "/arch:VFPv4")
  elseif(UNIX)
    set(GENERIC_CPU_FEATURE "-march=armv7-a+nosimd")
    set(NEON_CPU_FEATURE "-march=armv7-a+simd")
  endif()
    add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
    set_target_properties(next_start_code_c PROPERTIES COMPILE_FLAGS ${GENERIC_CPU_FEATURE} )
    target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_library(next_start_code_neon OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeNEON.cpp include)
    set_target_properties(next_start_code_neon PROPERTIES COMPILE_FLAGS ${NEON_CPU_FEATURE} )
    target_include_directories(next_start_code_neon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${VULKAN_VIDEO_PARSER_LIB} next_start_code_c next_start_code_neon)
else()
  if(WIN32)
    set(SSSE3_CPU_FEATURE "/arch:SSE2")
    set(AVX2_CPU_FEATURE "/arch:AVX2")
    set(AVX512_CPU_FEATURE "/arch:AVX512")
  elseif(UNIX)
    set(GENERIC_CPU_FEATURE "-mtune=generic -mno-sse -mno-sse2 -mno-ssse3 -mno-sse4 -mno-sse4.2 -mno-avx \
                             -mno-avx2 -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-bmi2")
    set(SSSE3_CPU_FEATURE "-march=nehalem")
    set(AVX2_CPU_FEATURE "-march=haswell")
    set(AVX512_CPU_FEATURE "-march=skylake-avx512")
  endif()
  add_library(next_start_code_c OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeC.cpp include)
  if(NOT WIN32)
    set_target_properties(next_start_code_c PROPERTIES COMPILE_FLAGS ${GENERIC_CPU_FEATURE} )
  endif()
  target_include_directories(next_start_code_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_ssse3 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeSSSE3.cpp include)
  set_target_properties(next_start_code_ssse3 PROPERTIES COMPILE_FLAGS ${SSSE3_CPU_FEATURE} )
  target_include_directories(next_start_code_ssse3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_avx2 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeAVX2.cpp include)
  set_target_properties(next_start_code_avx2 PROPERTIES COMPILE_FLAGS ${AVX2_CPU_FEATURE} )
  target_include_directories(next_start_code_avx2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_library(next_start_code_avx512 OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/src/NextStartCodeAVX512.cpp include)
  set_target_properties(next_start_code_avx512 PROPERTIES COMPILE_FLAGS ${AVX512_CPU_FEATURE} )
  target_include_directories(next_start_code_avx512 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
                RUNTIME DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
                ARCHIVE DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
                LIBRARY DESTINATION "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}"
        )

install(TARGETS ${VULKAN_VIDEO_PARSER_LIB} ${VULKAN_VIDEO_PARSER_STATIC_LIB}
                RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
