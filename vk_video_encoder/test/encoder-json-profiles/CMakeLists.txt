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
project(vk-video-encoder-json-load LANGUAGES CXX)

# The JSON loader and the configuration are linked from the encoder library's static archive, by
# file rather than by target, as the encoder DPB test links them: the target's link interface
# carries the shader compiler, which the loader does not need, and no Vulkan loader is needed.
add_executable(${PROJECT_NAME} src/JsonProfileLoad.cpp)
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

# Every JSON profile under vk_video_encoder/json_config validates against
# encoder_config.schema.json, loads through the loader --encoderConfig calls, and each setting it
# names, its tuning mode among them, reaches the configuration; a key the schema does not define
# and an unknown tuning mode are refused.
enable_testing()
add_test(NAME EncoderJsonProfiles
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/encoder_json_profiles.py
                 --tool $<TARGET_FILE:${PROJECT_NAME}>
                 --profiles ${CMAKE_CURRENT_SOURCE_DIR}/../../json_config
                 --schema ${CMAKE_CURRENT_SOURCE_DIR}/../../json_config/encoder_config.schema.json)
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderJsonProfiles PROPERTIES LABELS "device-free")

message(STATUS "vk-video-encoder-json-load: Configured")
