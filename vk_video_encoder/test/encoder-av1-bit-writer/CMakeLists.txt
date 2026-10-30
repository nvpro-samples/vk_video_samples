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
project(vk-video-encoder-av1-bit-writer LANGUAGES CXX)

# The AV1 encoder's bit writer (VkVideoEncoderAV1::VkVideoEncoderAV1BitWriter), which writes the
# show_existing_frame frame header OBU, is inline in its header: the test compiles it with the
# encoder's include paths and needs no library.
add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_BINARY_DIR}/generated/VkCodecUtils
    ${VULKAN_HEADERS_INCLUDE_DIR}
)
add_dependencies(${PROJECT_NAME} ${VULKAN_VIDEO_ENCODER_STATIC_LIB})

enable_testing()
add_test(NAME EncoderAv1BitWriter COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderAv1BitWriter PROPERTIES LABELS "device-free")
