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

project(encoder_ext_release_fence_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Only the PUBLIC API is used here -- unlike the sibling encoder-ext-sync
# test, this one has a real device and therefore needs no internal seams.
# The static archive is still what it links, so the binary can be staged to
# a GPU host on its own.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
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

# CTest semantics, DELIBERATELY different from the sibling encoder-ext-sync
# test. That one has no device by construction, so "could not stand up the
# session" is a real failure there. This one needs an encode-capable GPU, and
# a host without one has proved nothing either way -- so it exits 77 and is
# reported as SKIPPED, never as a pass. 0 means every assertion held, 1 means
# an assertion failed.
enable_testing()
# Two cases, because the two registration routings put the release fence on
# two DIFFERENT submissions: the staging copy (staged) and vkCmdEncodeVideoKHR
# (direct). A fence that only works on the staged arm silently answers -1 for
# every zero-copy producer, which is the arm this API exists for.
add_test(NAME EncoderExtReleaseFenceHwStaged
         COMMAND ${PROJECT_NAME})
add_test(NAME EncoderExtReleaseFenceHwDirect
         COMMAND ${PROJECT_NAME} direct)
# A third case about the pNext CHAIN rather than the queue: the same staged
# frame with the fence descriptor hung off a VkVideoEncoderFrameSyncDescriptor
# instead of off the submit info. The header documents a flat, order-
# independent chain; this is the only thing that proves the trailing position
# is really read, because a descriptor accepted by the sType gate and then
# never read is dead code that looks wired.
add_test(NAME EncoderExtReleaseFenceHwChained
         COMMAND ${PROJECT_NAME} chained)
set_tests_properties(EncoderExtReleaseFenceHwStaged
                     EncoderExtReleaseFenceHwDirect
                     EncoderExtReleaseFenceHwChained PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_release_fence_test: Configured")
