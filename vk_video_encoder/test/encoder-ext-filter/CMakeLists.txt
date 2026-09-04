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

project(encoder_ext_filter_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# Links the STATIC encoder library, not the shared one: the internal header's
# free functions (the format taxonomy, the config binder probe) are
# deliberately not exported from libvkvideo-encoder.so, so only the archive
# can satisfy them. This is also what makes the test a real one -- it links
# and calls the SAME object files the library ships, not a copy.
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

# The test branches on whether the preprocess compute filter is compiled in,
# and it must branch the SAME way the library did. Propagating the option
# here rather than re-deriving it is what keeps the two from disagreeing --
# a test that assumed the filter present against a library built without it
# would assert the wrong half and fail for the wrong reason.
if(BUILD_ENCODER_COMPUTE_FILTER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add tests.
#
# NOTE ON CTest SEMANTICS, matching the sibling library tests: 0 means every
# assertion held and 1 means an assertion failed. There is no GPU, driver or
# display dependence here -- every function under test reads only its
# arguments -- so there is no skip arm and a failure to run is a failure.
enable_testing()
add_test(NAME EncoderExtInputFormatTaxonomy
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderExtInputFormatTaxonomy PROPERTIES
                     LABELS "device-free")

message(STATUS "encoder_ext_filter_test: Configured")
