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

project(encoder_ext_import_ordinal_guard_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The STATIC archive, for the same reason encoder-ext-format-encode links it:
# the subject here is VkEncImportExternalImage, a free function declared in
# the INTERNAL header and deliberately not exported from
# libvkvideo-encoder.so. The archive also carries VulkanDeviceContext, which
# this test builds one of -- no encode session is created, because the guard
# is a property of the import and needs no encoder, no codec and no encode
# queue to exercise.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
    # This test includes vulkan_video_encoder_ext_internal.h, which is not on
    # the library target's interface. Naming the directory here is what a
    # legitimate internal consumer does, and what a client cannot.
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

# Must branch the same way the library did. Nothing in this test touches the
# filter, but the internal header is shared with the library's own TUs and a
# test compiled with a different view of it is a layout skew waiting to be
# blamed on something else.
if(BUILD_ENCODER_COMPUTE_FILTER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

enable_testing()

# ---------------------------------------------------------------------------
# THE SUBJECT, and what it asserts follows one library constant.
#
# THE GUARD IS RETIRED: kVkEncImportOrdinalGuardCount is 0, so this entry
# asserts it is INERT. A caller's first dma-buf import on a VkDevice must be
# the ONLY image created, nothing may be retained, the release must find
# nothing, and the verdict must be DISABLED. That is a gate and not a
# formality -- a build that re-arms the guard without telling this suite fails
# it in four independent places (image count, image identity, report, release).
#
# WHY IT IS DISABLED BY DEFAULT. The guard retains K sacrificial dma-buf
# imports so that caller imports land K live-positions later. Over enough
# import ordinals to cover several periods of the damage pattern, that changes
# WHICH imports the driver damages and never HOW MANY -- the damage rate is
# the same for every K. A short run that registers two or three buffers
# samples one period at one phase and reads the shift as a fix. There is also
# no consumer-side repair to fall back on: a write through a damaged import is
# swallowed, and an immediate re-import at the next ordinal does not rescue
# it. The defect is in the driver's dma-buf import path.
#
# THE ARMED BEHAVIOUR HAS NOT LOST ITS COVERAGE. The same binary takes
# --guard-count=N:
#
#     ./encoder_ext_import_ordinal_guard_test --guard-count=2
#
# against a library rebuilt with kVkEncImportOrdinalGuardCount = 2 runs every
# positional, retention and release assertion below exactly as it did when 2
# was the default. It is deliberately NOT registered as a ctest entry: the
# count is a build constant, so such an entry would be red against the library
# this tree actually builds. A knob whose test cannot run is not a tested
# knob, and this is how it stays testable in one command.
#
# WHAT MADE THIS SUITE NECESSARY. The workaround it covers shipped with a
# coverage floor of exactly zero: no test, CMake entry or CI file referenced
# it, and -- the reason a passing suite could not have caught that --
# VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF reached
# VkEncImportExternalImage from NO encoder-ext test at all. Every registration
# under vk_video_encoder/test was VK_IMAGE or OPAQUE_FD, on both of which
# VkEncEnsureImportOrdinalGuard returns early. The green suite was green with
# the guard deleted. It is also, now, what makes the retirement checkable:
# "the guard does nothing" is only a claim worth anything if something can
# tell the difference between that and "the guard is gone".
#
# WHAT IT ASSERTS, read off the DEVICE and not off a log line or a
# self-report: counting thunks are swapped into this test's own
# VulkanDeviceContext dispatch table, so the real driver entry points run and
# the library's calls are counted and ORDERED. With K the expected count (0 by
# default, --guard-count=N otherwise):
#
#   * K+1 vkCreateImage and K+1 fd-importing vkAllocateMemory for ONE caller
#     import, and the caller's VkImage is the one created last. At K = 0 that
#     is "exactly one image, and it is the caller's" -- the inertness claim.
#     At K > 0 it is the positional claim, "caller imports now start at
#     live-position K+1", stated in the only terms the driver understands.
#   * 0 vkDestroyImage and 0 vkFreeMemory before the release. At K > 0 this is
#     what separates the mechanism from a decoration: the original cross-tab
#     has a row for "2, created then DESTROYED" and it is 747/747 BAD, so a
#     guard that freed its pair would satisfy every count above while not even
#     delivering the phase shift it claims.
#   * a SECOND caller import costs exactly one more image, not K+1 -- the
#     documented "no-op after the first one, no per-frame cost".
#   * and the guard comes DOWN: VkEncReleaseImportOrdinalGuard returns K AND K
#     vkDestroyImage / K vkFreeMemory actually reach the driver, and a second
#     call returns 0 and destroys nothing. That function had no caller on any
#     test path outside this suite -- its production call site is inside a full
#     encode session's Deinitialize(), and no session in the corpus performs a
#     dma-buf import -- so the vkDestroyDevice-with-live-children leak it
#     closes could have come back with CI green. At K = 0 it must return 0 and
#     destroy nothing, which is the same assertion doing the retired build's
#     half of the job.
#
# AND IT CHECKS THE LIBRARY'S OWN VERDICT AGAINST THOSE COUNTS.
# VkEncImportOrdinalGuardReport must read DISABLED with retainedCount 0 at
# K = 0, COMPLETE with retainedCount == K above it -- and retainedCount must
# EQUAL (images created - the caller's one) either way. The DISABLED
# expectation is load-bearing rather than cosmetic: 0 == 0 satisfies the
# arithmetic behind COMPLETE, whose public definition is that caller imports
# landed past a position they did not, at 0, move to. A library that reports a
# working workaround on a build which removed it fails here.
#
# WHAT IT DOES NOT ASSERT, stated so a green run is not over-read: nothing
# about the pixels, and nothing about the driver. It does not measure whether
# imports are still damaged. They are -- that is why K is 0 -- and this suite
# would be green either way. It gates the MECHANISM and the build's claim
# about it, which is all a 900-second CI binary can honestly gate.
#
# SKIP_RETURN_CODE 77, like every sibling, and the skip is narrow: no Vulkan
# device, no dma-buf export, or a non-NVIDIA GPU -- where the guard refuses to
# run BY DESIGN, so there is nothing to prove and PASSED would be a lie. A
# failed assertion is 1 and can never be laundered into a skip: main() returns
# 1 on g_failures before it considers anything else.
# ---------------------------------------------------------------------------
add_test(NAME EncoderExtImportOrdinalGuardIsInertWhenRetired
         COMMAND ${PROJECT_NAME} --guard-count=0)
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
# --guard-count=0 is passed explicitly although it is the default, so the
# expectation this entry encodes is readable at the gate and not only in the
# binary.
set_tests_properties(EncoderExtImportOrdinalGuardIsInertWhenRetired
                     PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900
                     SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# THE KILL SWITCH, which had no coverage either -- and this entry is also the
# PERMANENT form of this suite's mutation proof, WITH ONE CAVEAT NOW: against
# the retired default it and the subject assert the same observable, because a
# build count of 0 and the kill switch produce the same inert shape. It
# regains its discriminating power the moment the library is built with a
# non-zero count, which is exactly when the guard is doing something worth
# switching off. It is kept registered because the kill switch is a shipped,
# documented control a support engineer will be told to set, and "setting it
# still stops the workaround" is a claim with no other test.
#
# The mutation that proves the subject above can go red is to disable the
# guard, and the cheapest lever for that is the documented escape hatch
# VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD=1. Against a library built with
# guard count 2, the four arms answer as follows; run --guard-count=2 against
# such a build to reproduce it:
#
#   arm               environment   exit   checks
#   ------------------------------------------------------------------------
#   (subject)         -             0      0 of 21 failed
#   (subject)         KILL=1        1      8 of 21 failed
#   --expect-disabled KILL=1        0      0 of 20 failed
#   --expect-disabled -             1      7 of 20 failed
#
# The guard-off failures name the property that broke rather than reporting a
# bare non-zero exit:
#       FAIL vkCreateImage call count ... expected 3, got 1
#       FAIL fd-importing vkAllocateMemory count ... expected 3, got 1
#       FAIL the caller's VkImage is the one created LAST, at live-position 3
#       FAIL the guard report's state is COMPLETE : got DISABLED
#
# AND A SOURCE-LEVEL MUTATION, because the env var only proves sensitivity to
# the env var. Deleting the VkEncEnsureImportOrdinalGuard() call from the top
# of VkEncImportExternalImage and rebuilding turns BOTH entries red --
# `ctest -R EncoderExtImportOrdinalGuard` exits 8, 0 of 2 passed, with the
# report reading NOT_EVALUATED on both. Restoring the file (md5 back to the
# original, binary md5 changed, so the restore really did rebuild) returns
# 2 of 2 passed, exit 0.
#
# The RELEASE half is mutation-proven separately, because a return value is
# not a destroy: neutering only the destroy loop inside
# VkEncReleaseImportOrdinalGuard -- so it still returns 2 while calling
# nothing -- turns the subject red with
#   FAIL the release destroyed exactly the guard's images and memories :
#        vkDestroyImage 0 -> 0, vkFreeMemory 0 -> 0, expected +2 each
# and nothing else, which is the assertion doing exactly its own job.
# Restoring (source md5 981b738a2656c7a61bda426608ace93f, test-binary md5
# 8be4cfb7... -> 62489f58..., so the rebuild happened) returns 2 of 2 passed.
#
# Note what the second row of that table means for THIS entry on its own: with
# the guard's call site deleted, "one import and it is the first" is trivially
# true, so the kill-switch arm's COUNT assertions cannot catch a deleted guard.
# Its report assertion can and does (DISABLED vs NOT_EVALUATED), and that is
# now the load-bearing half of BOTH entries: at the retired count the guard is
# supposed to create nothing, so only the verdict separates "retired" from
# "deleted". Deleting the VkEncEnsureImportOrdinalGuard() call from the top of
# VkEncImportExternalImage still turns both entries red, on
# NOT_EVALUATED-instead-of-DISABLED rather than on a count.
#
# This entry sets the variable through the test's ENVIRONMENT property and
# asserts the inert shape -- one image, and it is the first created. Against
# an ARMED library that is the OPPOSITE of the subject's assertions; run this
# binary with --guard-count=2 --expect-disabled and NO variable set, against
# such a library, and it fails, which is what stops the arm from being green
# for the wrong reason. Against the retired default there is no opposite left
# to assert, and the honest statement of what this entry then covers is: the
# kill-switch code path is still reached and still reports DISABLED.
#
# The variable is read once into a function-local static, so guard-on and
# guard-off cannot be exercised in one process -- hence two ctest entries over
# one binary rather than two arms in one run.
# ---------------------------------------------------------------------------
add_test(NAME EncoderExtImportOrdinalGuardKillSwitchDisablesIt
         COMMAND ${PROJECT_NAME} --expect-disabled)
set_tests_properties(EncoderExtImportOrdinalGuardKillSwitchDisablesIt
                     PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD=1")

message(STATUS "encoder_ext_import_ordinal_guard_test: Configured")
