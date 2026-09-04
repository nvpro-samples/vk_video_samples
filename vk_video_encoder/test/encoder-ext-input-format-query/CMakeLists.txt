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

project(encoder_ext_input_format_query_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Links the STATIC encoder library, as the sibling tests do, so the test calls
# the same object files the library ships rather than a copy.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    # The public encoder header reaches VkCodecUtils/VkVideoRefCountBase.h,
    # which lives under the shared common-libs root.
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# This file deliberately uses the PUBLIC header only. It is the one test in
# this tree that reaches the query the way a client would, so an internal
# include directory here would weaken exactly what it is measuring.

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

# Add test.
#
# A gpu-label test: every row puts a real profile to a real driver, and there
# is no arm of it that means anything without one. It exits 77 (SKIPPED) when
# no encode-capable NVIDIA device is enumerated, 1 when an assertion fails.
#
# VALIDATION-GATED, ALL THREE ROWS. A gate is easy to decline here on the
# ground that "nothing creates a session, an image or a command buffer -- the
# whole file is physical-device-level capability queries". That holds for the
# two query rows only. The --init-gate row drives one InitializeExt per advertised
# entry, and each of those runs CreateVideoEncoder, InitEncoder, the device
# format selection, the preprocess-filter build and the image-pool
# allocation. It is the highest-risk path in this directory, and the one that
# most needs a layer under it.
#
# THE TWO QUERY ROWS ARE GATED TOO, and that is not gate-everything reflex.
# vvs_add_validation_gated_test() reads the layer output ALONGSIDE the exit
# code, so it only adds a way to fail; a row with no spec-violation surface
# gates at zero messages and stays there, and the summary line it writes is
# what shows the layer was actually loaded. The old note's worry -- that a
# gate advertises coverage the test does not have -- is answered by the
# summary file rather than by declining to measure: a run with no layer is
# recorded as NOT-MEASURED, never as clean.
enable_testing()

vvs_add_validation_gated_test(EncoderExtInputFormatPointQuery
    TARGET ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
# No SKIP_RETURN_CODE: the gate carries the skip itself, through
# SKIP_REGULAR_EXPRESSION, because the registered command is now the cmake -P
# wrapper whose exit code is 0 or 1 and never 77. Its own note says why that
# ordering matters -- SKIP_RETURN_CODE outranks every other verdict, so a run
# that emitted validation errors on its way to a skip would be recorded as
# Skipped.
set_tests_properties(EncoderExtInputFormatPointQuery PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 300)

# The cross-route sweep, registered separately because it asserts a different
# proposition: not "is this pair accepted" but "do the two surfaces that answer
# that question give the same answer". Same binary, same context bring-up, same
# SKIP-77 harness -- a second executable would duplicate all three to assert
# something neither copy owns.
vvs_add_validation_gated_test(EncoderExtInputFormatCrossRoute
    TARGET ${PROJECT_NAME}
    ARGS   --cross-route)
set_tests_properties(EncoderExtInputFormatCrossRoute PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 300)

# The initialisation gate, registered separately for the same reason and one
# step further out: --cross-route asserts that the two PRE-SESSION surfaces
# agree with each other, and this asserts that the SESSION agrees with them.
# It is the only row in this binary that creates encoder sessions -- one per
# advertised entry -- which is why it carries a longer timeout than its two
# siblings and why the note above no longer says this binary creates none.
vvs_add_validation_gated_test(EncoderExtInputFormatInitGate
    TARGET ${PROJECT_NAME}
    ARGS   --init-gate)
set_tests_properties(EncoderExtInputFormatInitGate PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_input_format_query_test: Configured")
