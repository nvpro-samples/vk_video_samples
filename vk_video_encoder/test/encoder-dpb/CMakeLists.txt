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
project(vk-video-encoder-dpb-trace LANGUAGES CXX)

# The DPB classes are linked from the encoder library's static archive, by file rather than by
# target: the target's link interface carries the shader compiler, which the DPB objects do not
# need. The DPB objects, the GOP structure and the AV1 encoder configuration resolve inside the
# archive and simdjson's static library with no Vulkan loader, which is what lets the tests carry
# the device-free label.
add_executable(${PROJECT_NAME} src/EncoderDpbTrace.cpp)
add_dependencies(${PROJECT_NAME} ${VULKAN_VIDEO_ENCODER_STATIC_LIB})
target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_BINARY_DIR}/generated/VkCodecUtils
    ${VULKAN_HEADERS_INCLUDE_DIR}
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    $<TARGET_FILE:${VULKAN_VIDEO_ENCODER_STATIC_LIB}> simdjson::simdjson)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# H.265 long-term reference pictures, which the encoder does not set, driven through VkEncDpbH265
# directly.
add_executable(vk-video-encoder-dpb-h265-long-term src/H265LongTermRefs.cpp)
add_dependencies(vk-video-encoder-dpb-h265-long-term ${VULKAN_VIDEO_ENCODER_STATIC_LIB})
target_include_directories(vk-video-encoder-dpb-h265-long-term PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_BINARY_DIR}/generated/VkCodecUtils
    ${VULKAN_HEADERS_INCLUDE_DIR}
)
target_link_libraries(vk-video-encoder-dpb-h265-long-term PRIVATE
    $<TARGET_FILE:${VULKAN_VIDEO_ENCODER_STATIC_LIB}> simdjson::simdjson)
install(TARGETS vk-video-encoder-dpb-h265-long-term RUNTIME DESTINATION bin)

# Add tests.
#
# WHAT THIS GATES. The encoder's DPB decisions for H.264, H.265 and AV1 over GOP configurations
# (IPPP, IBBBP open and closed, seven B frames, 1 to 16 references, frame_num, POC and order hint
# wrap): each trace equals its golden (or recorded md5), and scripts/encoder_dpb_model.py's
# mismatch categories equal the configuration's known list, each a defect the suite's table names.
# The model replays the H.264 sliding window and initial reference lists (8.2.5.3, 8.2.4.2), the
# H.265 reference picture set and initial lists (8.3.2, 8.3.4) and the AV1 reference buffers
# (7.20, 7.21), and checks that every reference slot holds the picture the encoder means, with
# that picture's Std reference information, and that no setup slot holds a picture that stays a
# reference.
#
# WHAT A GREEN HERE DOES NOT MEAN. Nothing is encoded and no device is involved. The harness makes
# the calls the encoder makes; a change to the encoder's ProcessDpb, EncodeFrame or frame queue
# that the harness does not follow is not seen.
#
# A build with assertions skips the configurations that stop at one: the runner asks the tool.
enable_testing()
foreach(codec H264 H265 Av1)
    string(TOLOWER ${codec} suite)
    add_test(NAME EncoderDpbSequencing${codec}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/encoder_dpb_suite.py
                     --tool $<TARGET_FILE:${PROJECT_NAME}>
                     --suite ${CMAKE_CURRENT_SOURCE_DIR}/suites/${suite}.json)
    # LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
    set_tests_properties(EncoderDpbSequencing${codec} PROPERTIES LABELS "device-free")
endforeach()

# The H.265 long-term reference sets and their marking (8.3.2): every picture in RefPicSetLtCurr
# or RefPicSetLtFoll is used for long-term reference, with more Foll than Curr entries too, and a
# reference picture in no set is unused.
add_test(NAME EncoderH265DpbLongTermFoll COMMAND vk-video-encoder-dpb-h265-long-term)
set_tests_properties(EncoderH265DpbLongTermFoll PROPERTIES LABELS "device-free")

message(STATUS "vk-video-encoder-dpb-trace: Configured")
