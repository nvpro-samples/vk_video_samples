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
project(refcounted_pool_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VulkanVideoReferenceCountedPool.h is a template over std::shared_ptr and
# includes no Vulkan header, so this test compiles it on its own and needs no
# device and no library.
add_executable(${PROJECT_NAME} src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# WHAT THIS GATES. VulkanVideoRefCountedPool, the pool behind the decoder's and
# the encoder's bitstream buffers: a node added checked out goes back to the
# pool when its caller's last reference is released, the decoder's and the
# encoder's GetBitstreamBuffer pattern stops creating nodes once the pool holds
# one per buffer in flight, and a pool built with the default size holds its
# template capacity.
#
# WHAT A GREEN HERE DOES NOT MEAN. No buffer is allocated and no device is
# involved, so it says nothing about how many buffers a real decode or encode
# keeps in flight.
#
# 0 means every expectation held, 1 means one missed.
enable_testing()
add_test(NAME RefCountedPoolCheckout
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(RefCountedPoolCheckout PROPERTIES
                     LABELS "device-free")

message(STATUS "refcounted_pool_test: Configured")
