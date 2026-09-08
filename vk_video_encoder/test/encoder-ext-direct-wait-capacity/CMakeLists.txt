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

# The PUBLIC API only, like the sibling release-fence and acquire-fd tests: a
# real device needs no internal seams, and in this case the seam that exists
# would actively mislead. VkEncProbeLastSubmitSync records at the NULL BACKEND,
# which sits above the two SetExternalInputFrame* call sites and therefore
# above the fixed-array assembly this test is about -- it would report the
# full merged count and pass on a library that truncates.
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

# CTest semantics, matching the siblings: 0 every assertion held, 1 an
# assertion failed, 77 no encode-capable GPU and therefore nothing proved
# either way -- reported as SKIPPED, never as a pass.
enable_testing()

# ---------------------------------------------------------------------------
# THE SUBJECT. Runs on a DIRECT-routed registration (non-LINEAR NV12 declaring
# VIDEO_ENCODE_SRC), which is the only routing whose submit assembles waits
# into the fixed 8-slot array.
#
# Three cases, and the third is why the first two cannot be satisfied by a fix
# that simply refuses more work than it likes:
#   A  capacity+1 caller waits, last one unsignalled -- a surplus wait must not
#      be silently discarded.
#   B  capacity caller waits plus an armed acquire fence -- 9 into 8, where the
#      9th is the imported acquire semaphore because the ext layer appends it
#      last. The header's "loses neither" promise, at the count nothing tested.
#   C  capacity-1 caller waits plus an acquire fence -- exactly 8, fits, and
#      must still be accepted and complete.
# ---------------------------------------------------------------------------
add_test(NAME EncoderExtDirectWaitArrayCapacity
         COMMAND ${PROJECT_NAME})
set_tests_properties(EncoderExtDirectWaitArrayCapacity PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu"
                     TIMEOUT 900)

# ---------------------------------------------------------------------------
# THE CONTROL, AND IT IS EVIDENCE RATHER THAN DECORATION.
#
# The identical capacity+1 case against a LINEAR/TRANSFER_SRC registration,
# which routes STAGED onto the std::vector assembly that never truncates. It
# must report the release fence UNSIGNALLED while the surplus wait is still at
# 0 -- i.e. the same apparatus, reading a wait that WAS honoured.
#
# This is what makes a green subject arm mean something. There is no public or
# seam accessor for a slot's resolved inputPath, so "did this registration
# really route DIRECT" cannot be asserted directly; the differential between
# these two entries is the substitute, and it is also the standing answer to
# "is this a test that can fail at all".
# ---------------------------------------------------------------------------
add_test(NAME EncoderExtDirectWaitArrayCapacityStagedControl
         COMMAND ${PROJECT_NAME} --staged)
set_tests_properties(EncoderExtDirectWaitArrayCapacityStagedControl PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu"
                     TIMEOUT 900)

# ---------------------------------------------------------------------------
# THE LEGACY ENTRY POINT, WHICH REACHES THE SAME FIXED ARRAY BY A DIFFERENT
# DOOR.
#
# SubmitExternalFrame carries a raw VkImage and no registration, so the
# encoder core routes it from the frame's own format and tiling: an OPTIMAL
# NV12 frame is directly encodable and its waits land in the same 8-slot
# array. The registered arms above cannot cover it -- they enter through
# SubmitRegisteredFrame, which is a different entry point with a different
# return type and its own bound.
#
# The readout here is COMPLETION rather than a release fence, because this
# entry point refuses any pNext chain and so can carry no fence descriptor.
# Every wait is pre-signalled, so an accepted frame that never becomes
# retrievable is a frame that was dropped after its caller was told it was
# taken -- a success that can only be waited on forever.
#
# Three cases, and the outer two are what stop a fix that merely refuses:
#   3 waits  -- inside capacity, must be accepted AND complete.
#   9 waits  -- over capacity, must be reported rather than accepted-and-lost,
#               and reported with the code the public header names.
#   8 waits  -- exactly full, legal input, must still be accepted AND
#               complete.
# ---------------------------------------------------------------------------
#
# GATED ON VALIDATION, because this arm and its H.265 sibling below are the
# only arms in the tree that code a B frame. consecutiveBFrames is 2 on the
# legacy arms and 0 everywhere else, so the encoder builds a bidirectional
# reference list here and nowhere else, and a spec violation on that path is
# reported by nothing unless these entries read the layer.
#
# THE CEILING BELOW NAMES A DEFECT, IT DOES NOT EXCUSE IT:
#
#   VUID-VkImageCreateInfo-pNext-06811
#       the direct input image is created with MUTABLE_FORMAT|EXTENDED_USAGE
#       and the full usage set the direct path may view it under, and the
#       driver reports no video format properties for that combination under
#       this profile. It is raised once, at the single vkCreateImage this
#       arm performs.
vvs_add_validation_gated_test(EncoderExtLegacySubmitWaitArrayCapacity
    TARGET ${PROJECT_NAME}
    ARGS   --legacy
    EXPECT VUID-VkImageCreateInfo-pNext-06811=1)
set_tests_properties(EncoderExtLegacySubmitWaitArrayCapacity PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

# ---------------------------------------------------------------------------
# THE SAME ARM UNDER H.265, BECAUSE THE REFERENCE LISTS ARE ASSEMBLED PER
# CODEC.
#
# VkVideoEncoderH264 and VkVideoEncoderH265 each fill referenceSlotsInfo[]
# from their own L0 and L1 walks, in separate files. A B frame exercised
# through one of them leaves the other's walk unread, so the codec is a
# parameter of this arm rather than a constant: --h265 selects an H.265
# session and chains VkVideoEncodeH265ProfileInfoKHR on the input image's
# profile, and everything else about the arm is unchanged.
#
# The ceiling is the same one, for the same reason, and names the same defect.
vvs_add_validation_gated_test(EncoderExtLegacySubmitWaitArrayCapacityH265
    TARGET ${PROJECT_NAME}
    ARGS   --legacy --h265
    EXPECT VUID-VkImageCreateInfo-pNext-06811=1)
set_tests_properties(EncoderExtLegacySubmitWaitArrayCapacityH265 PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_direct_wait_capacity_test: Configured")
