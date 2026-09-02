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

project(encoder_ext_format_matrix_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The STATIC library, for the same reason the sibling taxonomy test links it:
# the internal header's free functions -- the format taxonomy and
# VkEncProbeResource, which is how this test reads a slot's resolved input
# path -- are deliberately not exported from libvkvideo-encoder.so.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

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

# Must branch the same way the library did -- an RGBA session cannot route
# FILTER at all in a build without the filter, and a test that assumed it
# present would fail for the wrong reason.
if(BUILD_ENCODER_COMPUTE_FILTER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add test.
#
# Every row creates a real encode session and a real VkImage on the
# LIBRARY-OWNED device, so this is a gpu-label test: it exits 77 (SKIPPED)
# when no encode-capable Vulkan device is present, and 1 when an assertion
# fails. There is no arm of it that is meaningful without a device -- the
# device-free half of the taxonomy is already covered by
# EncoderExtInputFormatTaxonomy next door.
enable_testing()
#
# The matrix is walked by PROFILE FAMILY, one invocation each, because a row
# costs a whole encoder session and a process cannot hold an unbounded number
# of them. Splitting is not a convenience: past that limit a row reports "this
# device has no such encode profile" for a profile the device does support,
# and the suite would go green over a wrong finding.
add_test(NAME EncoderExtInputFormatMatrix
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderExtInputFormatMatrix PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

# Semi-planar 4:4:4, which belongs to a different profile in both codecs
# (H.264 High 4:4:4 Predictive, H.265 Range Extensions) and so to a different
# family. Carries the same two controls, so a refusal is observed on the same
# engine that ran the 4:4:4 rows.
add_test(NAME EncoderExtInputFormatMatrix444
         COMMAND ${PROJECT_NAME} --444)
set_tests_properties(EncoderExtInputFormatMatrix444 PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu")

message(STATUS "encoder_ext_format_matrix_test: Configured")
