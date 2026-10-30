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
project(vk-video-encoder-psnr-sum LANGUAGES CXX)

# The encoder's PSNR sum (VkVideoEncoderPsnrSum) is defined in its header and includes no Vulkan
# header: the test compiles it alone and needs no library and no device.
add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
)

# So is the plane PSNR the sum adds (VkVideoEncoderPlanePsnr): each sample read as a bit-depth
# value, from a file's layout and the reconstruction's.
add_executable(vk-video-encoder-plane-psnr src/plane_psnr.cpp)
target_include_directories(vk-video-encoder-plane-psnr PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}/../libs
)

enable_testing()
add_test(NAME EncoderPsnrSumOrderIndependent COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
add_test(NAME EncoderPsnrKernel COMMAND $<TARGET_FILE:vk-video-encoder-plane-psnr>)
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderPsnrSumOrderIndependent EncoderPsnrKernel PROPERTIES LABELS "device-free")
