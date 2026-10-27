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
project(vk-video-parser-trace LANGUAGES CXX)

# The parser is linked from its static archive: the tool needs no Vulkan loader, no device and no
# shared library, which is what lets the test carry the device-free label.
add_executable(${PROJECT_NAME} src/ParserTrace.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_PARSER_INCLUDE}/..
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_HEADERS_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${VULKAN_VIDEO_PARSER_STATIC_LIB})

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# WHAT THIS GATES. Every stream in suites/<codec>.json that is available runs through the parser,
# and these checks apply:
# - golden: the trace equals the recorded golden, so any change in what the parser hands a decoder
#   fails until the golden is re-recorded with that change explained. Streams outside the tree
#   (the decode samples, conformance corpora) are held to the md5 of their trace instead;
# - model, AV1: scripts/av1_reference_model.py replays the AV1 reference state of the
#   specification (AV1 5.9.2, 7.8, 7.20, 7.21) from the stream's headers and compares the trace's
#   picture, segmentation and reference information with it. Each stream lists the mismatch
#   categories of known defects; a category that appears unlisted fails, and a listed one that no
#   longer appears fails too, so a fix has to remove its entry.
#
# The committed streams under streams/ carry their AV1 header dumps, so no check needs ffmpeg.
# Streams under an environment-variable root run only when that variable is set.
#
# WHAT A GREEN HERE DOES NOT MEAN. Nothing is decoded. The test shows what the parser hands a
# client; whether a driver decodes it correctly is the GPU suites' question.
enable_testing()
foreach(codec Av1 H264 H265 Vp9)
    string(TOLOWER ${codec} suite)
    add_test(NAME ParserTrace${codec}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/parser_trace_suite.py
                     --tool $<TARGET_FILE:${PROJECT_NAME}>
                     --suite ${CMAKE_CURRENT_SOURCE_DIR}/suites/${suite}.json)
    # LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
    set_tests_properties(ParserTrace${codec} PROPERTIES LABELS "device-free")
endforeach()

message(STATUS "vk-video-parser-trace: Configured")
