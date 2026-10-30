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

# Fails unless vk_filter_test --list prints every case function TestCases.cpp defines, either in a
# set or among the disabled cases with its defect, and no disabled case is also in a set. A case
# that no set names and no disabled entry names is never run and never reported.
#
#   cmake -DFILTER_TEST=<vk_filter_test> -DTEST_CASES_CPP=<TestCases.cpp> -P filter_test_list.cmake

foreach(var FILTER_TEST TEST_CASES_CPP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "filter_test_list.cmake needs -D${var}=...")
    endif()
endforeach()

# The case functions: "TestCaseConfig <name>() {", one definition per line.
file(STRINGS "${TEST_CASES_CPP}" definitions
     REGEX "^TestCaseConfig[ \t]+TC[A-Za-z0-9_]+[ \t]*\\([ \t]*\\)[ \t]*{")
set(defined "")
foreach(line ${definitions})
    string(REGEX REPLACE "^TestCaseConfig[ \t]+(TC[A-Za-z0-9_]+).*$" "\\1" name "${line}")
    list(APPEND defined "${name}")
endforeach()
if(NOT defined)
    message(FATAL_ERROR "No case function in ${TEST_CASES_CPP}")
endif()

execute_process(COMMAND "${FILTER_TEST}" --list
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${FILTER_TEST} --list exited ${rc}:\n${out}${err}")
endif()

# One list item per output line: a semicolon or a square bracket in a defect would split or join
# lines.
string(REGEX REPLACE "[][;]" "_" out "${out}")
string(REPLACE "\n" ";" lines "${out}")
set(section "")
set(sawDisabled FALSE)
set(inSets "")
set(disabled "")
set(errors "")
foreach(line ${lines})
    if(line MATCHES "^=== (.+) ===$")
        set(section "${CMAKE_MATCH_1}")
        if(section STREQUAL "DISABLED TESTS")
            set(sawDisabled TRUE)
        endif()
    elseif(line MATCHES "^  (TC[A-Za-z0-9_]+)(.*)$")
        set(name "${CMAKE_MATCH_1}")
        set(rest "${CMAKE_MATCH_2}")
        if(section STREQUAL "DISABLED TESTS")
            if(NOT rest MATCHES "^: [^ ]")
                string(APPEND errors "  disabled case ${name} names no defect\n")
            endif()
            list(APPEND disabled "${name}")
        elseif(section STREQUAL "")
            string(APPEND errors "  ${name} is listed before any set\n")
        else()
            list(APPEND inSets "${name}")
        endif()
    endif()
endforeach()
if(NOT sawDisabled)
    string(APPEND errors "  --list prints no DISABLED TESTS section\n")
endif()

list(REMOVE_DUPLICATES inSets)
foreach(name ${defined})
    list(FIND inSets "${name}" inSet)
    list(FIND disabled "${name}" isDisabled)
    if(inSet EQUAL -1 AND isDisabled EQUAL -1)
        string(APPEND errors "  ${name} is defined, in no set and not disabled\n")
    elseif(NOT inSet EQUAL -1 AND NOT isDisabled EQUAL -1)
        string(APPEND errors "  ${name} is disabled and in a set\n")
    endif()
endforeach()
foreach(name ${inSets} ${disabled})
    list(FIND defined "${name}" isDefined)
    if(isDefined EQUAL -1)
        string(APPEND errors "  ${name} is listed, but no case function has that name\n")
    endif()
endforeach()

if(errors)
    message(FATAL_ERROR "vk_filter_test --list does not match ${TEST_CASES_CPP}:\n${errors}")
endif()
list(LENGTH defined definedCount)
list(LENGTH inSets inSetCount)
list(LENGTH disabled disabledCount)
message(STATUS "${definedCount} cases defined: ${inSetCount} in a set, ${disabledCount} disabled")
