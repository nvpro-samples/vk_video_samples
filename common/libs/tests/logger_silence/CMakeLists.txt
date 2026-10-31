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
project(logger_silence_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Logger.h is header-only and LoggerSilence.cpp includes no Vulkan header, so this test needs no device and
# no library.
add_executable(${PROJECT_NAME}
    src/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../VkCodecUtils/LoggerSilence.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# WHAT THIS GATES. LoggerSilenceScope, behind the encoder's silenceStdio: while a scope that requests silence
# lives the Logger prints nothing, nested scopes are counted, and the last release gives back the level the
# Logger had before the first, whatever it was.
#
# 0 means every expectation held, 1 means one missed.
enable_testing()
add_test(NAME LoggerSilenceScopes
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(LoggerSilenceScopes PROPERTIES
                     LABELS "device-free")

message(STATUS "logger_silence_test: Configured")
