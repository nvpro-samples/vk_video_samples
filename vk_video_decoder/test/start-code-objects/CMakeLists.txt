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

# The parser's start code scanners. Every scanner the CPU runs gives the C scanner's results
# (ParserStartCodeKernelsAgree). Each object built with instruction set flags defines its kernel and
# nothing else (ParserStartCodeObjectsKernelsOnly), and on x86-64 no function of the parser library
# but the AVX2 and AVX-512 kernels uses an AVX instruction (ParserAvxOnlyInStartCodeKernels); these
# two read the build's objects with the toolchain's nm and objdump, so they are defined where those
# are GNU or LLVM tools, which MSVC is not.

set(PARSER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/NvVideoParser")
set(kernelObjects "")
set(isaObjects "")
foreach(isa C SSSE3 AVX2 AVX512 NEON SVE)
    string(TOLOWER ${isa} target)
    if(TARGET next_start_code_${target})
        list(APPEND kernelObjects $<TARGET_OBJECTS:next_start_code_${target}>)
        if(NOT isa STREQUAL "C")
            list(APPEND isaObjects "$<JOIN:$<TARGET_OBJECTS:next_start_code_${target}>,|>|NextStartCode${isa}")
        endif()
    endif()
endforeach()
if(NOT kernelObjects)
    return()
endif()

enable_testing()

add_executable(start_code_kernels_test src/kernels_agree.cpp ${PARSER_DIR}/src/cpudetect.cpp ${kernelObjects})
target_include_directories(start_code_kernels_test PRIVATE ${PARSER_DIR}/include)
add_test(NAME ParserStartCodeKernelsAgree COMMAND start_code_kernels_test)
set_tests_properties(ParserStartCodeKernelsAgree PROPERTIES LABELS "device-free")

if(MSVC OR NOT CMAKE_NM OR NOT CMAKE_OBJDUMP OR NOT isaObjects)
    return()
endif()
string(REPLACE ";" "," objectsArg "${isaObjects}")

add_test(NAME ParserStartCodeObjectsKernelsOnly
         COMMAND ${CMAKE_COMMAND} -DMODE=objects -DNM=${CMAKE_NM} "-DOBJECTS=${objectsArg}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_start_code_objects.cmake)
set_tests_properties(ParserStartCodeObjectsKernelsOnly PROPERTIES LABELS "device-free")

if(TARGET next_start_code_avx2)
    add_test(NAME ParserAvxOnlyInStartCodeKernels
             COMMAND ${CMAKE_COMMAND} -DMODE=library -DOBJDUMP=${CMAKE_OBJDUMP}
                     -DLIBRARY=$<TARGET_FILE:${VULKAN_VIDEO_PARSER_LIB}>
                     -DALLOWED=NextStartCodeAVX2,NextStartCodeAVX512
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_start_code_objects.cmake)
    set_tests_properties(ParserAvxOnlyInStartCodeKernels PROPERTIES LABELS "device-free")
endif()
