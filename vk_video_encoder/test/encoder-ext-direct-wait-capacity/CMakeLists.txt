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

project(encoder_ext_direct_wait_capacity_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The descriptor API and no internal seam: a real device needs none, and the
# fixed-array assembly this test is about sits below anything a seam records.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    # encode_device_probe.h, which decides whether a library failure is a skip.
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    # The descriptor API is an internal header: the public surface of this
    # library is the encoder interface, and a test that drives the layer
    # beneath it names the internal directory to say so.
    ${VULKAN_VIDEO_ENCODER_INTERNAL_INCLUDE}
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

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# CTest semantics: 0 every assertion held, 1 an
# assertion failed, 77 no encode-capable GPU and therefore nothing proved
# either way -- reported as SKIPPED, never as a pass.
enable_testing()

# ---------------------------------------------------------------------------
# SubmitExternalFrame carries a raw VkImage, and the encoder core routes it
# from the frame's own format and tiling: an OPTIMAL NV12 frame is directly
# encodable and its waits land in the fixed 8-slot array.
#
# The readout is COMPLETION. Every wait is pre-signalled, so an accepted frame
# that never becomes retrievable is a frame that was dropped after its caller
# was told it was taken -- a success that can only be waited on forever.
#
# Three cases, and the outer two are what stop a fix that merely refuses:
#   3 waits  -- inside capacity, must be accepted AND complete.
#   9 waits  -- over capacity, must be reported rather than accepted-and-lost,
#               and reported with the code the header names.
#   8 waits  -- exactly full, legal input, must still be accepted AND
#               complete.
# ---------------------------------------------------------------------------
#
# GATED ON VALIDATION, because this entry and its H.265 sibling below are the
# only entries in the tree that code a B frame through the descriptor API.
# consecutiveBFrames is 2 here, so the encoder builds a bidirectional
# reference list, and a spec violation on that path is reported by nothing
# unless these entries read the layer.
#
# THE CEILING BELOW NAMES A DEFECT, IT DOES NOT EXCUSE IT:
#
#   VUID-VkImageCreateInfo-pNext-06811
#       the direct input image is created with MUTABLE_FORMAT|EXTENDED_USAGE
#       and the full usage set the direct path may view it under, and the
#       driver reports no video format properties for that combination under
#       this profile. It is raised once, at the single vkCreateImage this
#       test performs.
vvs_add_validation_gated_test(EncoderExtLegacySubmitWaitArrayCapacity
    TARGET ${PROJECT_NAME}
    EXPECT VUID-VkImageCreateInfo-pNext-06811=1)
set_tests_properties(EncoderExtLegacySubmitWaitArrayCapacity PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

# ---------------------------------------------------------------------------
# THE SAME CASES UNDER H.265, BECAUSE THE REFERENCE LISTS ARE ASSEMBLED PER
# CODEC.
#
# VkVideoEncoderH264 and VkVideoEncoderH265 each fill referenceSlotsInfo[]
# from their own L0 and L1 walks, in separate files. A B frame exercised
# through one of them leaves the other's walk unread, so the codec is a
# parameter of this test rather than a constant: --h265 selects an H.265
# session and chains VkVideoEncodeH265ProfileInfoKHR on the input image's
# profile, and everything else is unchanged.
#
# The ceiling is the same one, for the same reason, and names the same defect.
vvs_add_validation_gated_test(EncoderExtLegacySubmitWaitArrayCapacityH265
    TARGET ${PROJECT_NAME}
    ARGS   --h265
    EXPECT VUID-VkImageCreateInfo-pNext-06811=1)
set_tests_properties(EncoderExtLegacySubmitWaitArrayCapacityH265 PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_direct_wait_capacity_test: Configured")
