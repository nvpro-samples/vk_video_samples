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

project(encoder_h264_baseline_entropy_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Links the STATIC encoder library: EncoderConfigH264 and its parameter-set
# writers live below the ext layer and are not exported from
# libvkvideo-encoder.so.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    ${VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Vulkan is loaded at runtime (VK_NO_PROTOTYPES), so only headers are needed.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND TARGET Vulkan::Vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
elseif(TARGET vulkan)
    target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread dl)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add tests.
#
# CTest semantics, matching the sibling library tests: 0 means every assertion
# held, 1 means an assertion failed, and 2 means the session could not be stood
# up at all -- deliberately a FAILURE and not a skip.
#
# WHAT THIS GATES. H.264 Baseline and Constrained Baseline are both profile_idc
# 66 and neither admits CABAC; entropy_coding_mode_flag must be 0. Nothing tied
# the entropy coder to the profile, and the entropy coder is not chosen by the
# caller at all -- there is no switch for it, it is taken from the device
# preferredStdEntropyCodingModeFlag, which is CABAC on this vendor hardware. So
# an explicit --profile baseline emitted profile_idc 66 with
# entropy_coding_mode_flag 1: a non-conformant bitstream, and reachable from the
# WebRTC default profile.
#
# Three parts: the rule over the whole profile x entropy matrix, the emitted
# SPS/PPS pair, and the composition with the profile auto-upgrade in
# InitProfileLevel() -- which does NOT cover this case, because it is guarded by
# profileIdc == INVALID and so never runs on an explicit request. That last part
# is the one worth having: it is the reason the guard is not redundant with the
# upgrade, and it fails if someone later widens the upgrade to fire on requests
# and assumes it subsumes the clamp.
#
# WHAT A GREEN HERE DOES NOT MEAN: it does not mean a decoder accepted the
# stream. This asserts the syntax elements the writers produce, not a decode.
# Conformance against a real decoder needs hardware and is proven there.
enable_testing()
add_test(NAME EncoderH264BaselineRejectsCabac
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderH264BaselineRejectsCabac PROPERTIES
                     LABELS "device-free")

message(STATUS "encoder_h264_baseline_entropy_test: Configured")
