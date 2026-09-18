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

project(encoder_sync_assembly_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Links the STATIC encoder library, not the shared one: this TU subclasses
# VkVideoEncoder itself, and none of that class is exported from
# libvkvideo-encoder.so.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    # VkVideoEncoder/VkVideoEncoder.h and VkVideoEncoder/VkEncoderConfigH264.h
    # are library-internal headers, not part of the installed include dir.
    # This is the same root the library's own TUs compile against.
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
    # The encoder header reaches VkCodecUtils/* and mio/mio.hpp under the
    # shared common-libs root.
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    # nvidia_utils/vulkan/ycbcrvkinfo.h resolves from the repository root.
    ${CMAKE_CURRENT_LIST_DIR}/../../..
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
# NOTE ON CTest SEMANTICS, matching the sibling library tests: 0 means every
# assertion held and 1 means an assertion failed. There is NO skip code here
# and deliberately so -- the subject has no GPU, driver, display or input-file
# dependence of any kind, so a host that could not run it is a broken host,
# not an unsupported one.
enable_testing()
add_test(NAME EncoderSyncAssemblyPublishesNoCompletionRecord
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderSyncAssemblyPublishesNoCompletionRecord PROPERTIES
                     LABELS "device-free")

message(STATUS "encoder_sync_assembly_test: Configured")
