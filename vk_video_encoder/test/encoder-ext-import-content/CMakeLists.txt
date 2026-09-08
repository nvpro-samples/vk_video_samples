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

project(encoder_ext_import_content_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Links the STATIC encoder library, not the shared one: this reaches both the
# internal null-backend seam and VkVideoEncoderContentProbe, neither of which
# is exported from libvkvideo-encoder.so.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    # This test includes vulkan_video_encoder_ext_internal.h, which is not on
    # the library target's interface. Naming the directory here is what a
    # legitimate internal consumer does, and what a client cannot.
    ${VULKAN_VIDEO_ENCODER_INTERNAL_INCLUDE}
    # VkVideoEncoder/VkVideoEncoderContentProbe.h -- the probe class lives in
    # the encoder library, below the ext layer, and this suite drives its pure
    # scorer and predicate directly.
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
# WHAT A GREEN HERE DOES NOT MEAN, stated at the gate and not only in the
# source: it does NOT mean the workaround fires on a buffer the driver
# actually poisoned. That needs a driver that exhibits the defect, a GBM
# exporter and a real dma-buf import, and is proven on hardware or not at
# all.
#
# WHAT IT DOES GATE, and it is the part that would otherwise be provable on
# exactly one machine: the DECISION. Given the bytes a poisoned import
# produces -- which this suite synthesises, laid out with the plane offsets,
# padded row pitch and skipped rows a real HOST_VISIBLE LINEAR image has --
# the predicate, the scorer, the per-registration latch, the
# oldest-damaged-first report ordering, the drain on retirement and the
# chained carrier are all exercised with no GPU. The predicate in particular
# is asserted over EVERY (Y,U,V) liveness quadrant rather than spot-checked,
# because the chroma-only version of it has produced a wrong conclusion on
# this defect twice, and a fully legal BLACK frame (Y=16, U=V=128) is
# asserted CLEAN, because that is the false positive that would reroute
# working buffers.
enable_testing()
add_test(NAME EncoderExtImportContentProbe
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderExtImportContentProbe PROPERTIES
                     LABELS "device-free")

message(STATUS "encoder_ext_import_content_test: Configured")
