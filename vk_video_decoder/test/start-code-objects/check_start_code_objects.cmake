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

# The parser's start code scanners built with instruction set flags hold their kernels and nothing
# else, and no other code of the parser library uses the instructions only those flags allow.
#
#   cmake -DMODE=objects -DNM=<nm> -DOBJECTS=<object>|<kernel name>[,<object>|<kernel name>...]
#         -P check_start_code_objects.cmake
#     Fails on a weak or unique symbol in an object, and on any global symbol but its kernel: an
#     inline function or a template instance the object defines is built with the object's flags,
#     and the linker may keep that copy for every caller.
#   cmake -DMODE=library -DOBJDUMP=<objdump> -DLIBRARY=<parser library> -DALLOWED=<kernel>[,<kernel>...]
#         -DWORK_DIR=<dir> -P check_start_code_objects.cmake
#     Fails on an AVX, AVX2 or AVX-512 instruction (VEX or EVEX encoded: a v-prefixed mnemonic, or a
#     ymm, zmm or mask register) in any function of the library but the kernels named.

cmake_minimum_required(VERSION 3.20)

if(MODE STREQUAL "objects")
    foreach(var NM OBJECTS)
        if(NOT DEFINED ${var})
            message(FATAL_ERROR "check_start_code_objects.cmake (objects) needs -D${var}=...")
        endif()
    endforeach()
    set(errors "")
    string(REPLACE "," ";" OBJECTS "${OBJECTS}")
    foreach(entry ${OBJECTS})
        string(REPLACE "|" ";" pair "${entry}")
        list(GET pair 0 object)
        list(GET pair 1 kernel)
        execute_process(COMMAND "${NM}" --defined-only -C "${object}"
                        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${NM} ${object} exited ${rc}:\n${err}")
        endif()
        string(REGEX REPLACE "[][;]" "_" out "${out}")
        string(REPLACE "\n" ";" lines "${out}")
        set(sawKernel FALSE)
        foreach(line ${lines})
            if(NOT line MATCHES "^[0-9a-fA-F]* *([A-Za-z]) (.*)$")
                continue()
            endif()
            set(type "${CMAKE_MATCH_1}")
            set(name "${CMAKE_MATCH_2}")
            if(type MATCHES "^[WwVvu]$")
                string(APPEND errors "  ${object}: weak or unique ${type} ${name}\n")
            elseif(type MATCHES "^[A-Z]$")
                if(type STREQUAL "T" AND name MATCHES "^${kernel}\\(")
                    set(sawKernel TRUE)
                else()
                    string(APPEND errors "  ${object}: global ${type} ${name}\n")
                endif()
            endif()
        endforeach()
        if(NOT sawKernel)
            string(APPEND errors "  ${object}: no definition of ${kernel}\n")
        endif()
    endforeach()
    if(errors)
        message(FATAL_ERROR "A start code object defines more than its kernel:\n${errors}")
    endif()
    list(LENGTH OBJECTS count)
    message(STATUS "${count} start code objects define their kernels and nothing else")
elseif(MODE STREQUAL "library")
    foreach(var OBJDUMP LIBRARY ALLOWED WORK_DIR)
        if(NOT DEFINED ${var})
            message(FATAL_ERROR "check_start_code_objects.cmake (library) needs -D${var}=...")
        endif()
    endforeach()
    string(REPLACE "," ";" ALLOWED "${ALLOWED}")
    file(MAKE_DIRECTORY "${WORK_DIR}")
    set(dump "${WORK_DIR}/parser-library.dis")
    execute_process(COMMAND "${OBJDUMP}" -d -C --no-show-raw-insn "${LIBRARY}"
                    OUTPUT_FILE "${dump}" ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${OBJDUMP} ${LIBRARY} exited ${rc}:\n${err}")
    endif()
    # Function headers ("<address> <name>:") and the VEX or EVEX encoded instructions only.
    file(STRINGS "${dump}" lines REGEX "(^[0-9a-f]+ <.*>:$)|(%[yz]mm[0-9])|(%k[0-7])|(:[ \t]+v[a-z0-9]+[ \t])")
    set(function "")
    set(offenders "")
    set(kernelsWithAvx "")
    foreach(line ${lines})
        if(line MATCHES "^[0-9a-f]+ <(.*)>:$")
            set(function "${CMAKE_MATCH_1}")
            continue()
        endif()
        set(allowed FALSE)
        foreach(kernel ${ALLOWED})
            string(FIND "${function}" "${kernel}(" at)
            if(at EQUAL 0)
                set(allowed TRUE)
            endif()
        endforeach()
        if(allowed)
            list(APPEND kernelsWithAvx "${function}")
        else()
            list(APPEND offenders "${function}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES offenders)
    list(REMOVE_DUPLICATES kernelsWithAvx)
    if(offenders)
        string(REPLACE ";" "\n  " list "${offenders}")
        message(FATAL_ERROR "AVX instructions outside the start code kernels in ${LIBRARY}:\n  ${list}")
    endif()
    list(LENGTH kernelsWithAvx count)
    message(STATUS "AVX instructions only in the start code kernels (${count} of them use any)")
else()
    message(FATAL_ERROR "check_start_code_objects.cmake needs -DMODE=objects or -DMODE=library")
endif()
