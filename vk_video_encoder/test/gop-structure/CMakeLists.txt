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
project(gop_structure_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VkVideoGopStructure.cpp is compiled in rather than linked from the encoder
# library on purpose: the class it defines includes no Vulkan header and needs
# no device, and compiling it here keeps this test free of the library's link
# closure. That is what lets it carry the device-free label honestly.
add_executable(${PROJECT_NAME}
    src/main.cpp
    ${CMAKE_SOURCE_DIR}/vk_video_encoder/libs/VkVideoEncoder/VkVideoGopStructure.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# Add tests.
#
# WHAT THIS GATES. The GOP machine's frame sequencing for one configuration --
# gopFrameCount 11, idrPeriod 25, 3 consecutive B frames, open GOP -- checked
# frame by frame over 30 frames against three recorded tables: the encode
# order, the B-frame count at each position, and the B-frame position within
# each mini-GOP. It also asserts that FLAGS_CLOSE_GOP is never set on an IDR
# in an open-GOP configuration.
#
# WHAT A GREEN HERE DOES NOT MEAN. Nothing is encoded and no device is
# involved. This is the sequencing decision only, taken before any picture
# exists, so a green says the order the encoder intends is the order recorded
# -- not that the encoder coded it, and not that any other GOP configuration
# sequences correctly. printGopTable() and testEdgeCases() in the same binary
# print and assert nothing; they are not part of the gate.
#
# CTest semantics, matching the sibling library tests: 0 means every
# expectation held, 1 means one missed.
enable_testing()
add_test(NAME EncoderGopStructureSequencing
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderGopStructureSequencing PROPERTIES
                     LABELS "device-free")

message(STATUS "gop_structure_test: Configured")
