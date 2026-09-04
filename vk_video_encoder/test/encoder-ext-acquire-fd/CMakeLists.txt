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

project(encoder_ext_acquire_fd_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The PUBLIC API only, like the sibling release-fence test -- a real device
# needs no internal seams. The STATIC archive is what it links, which is also
# what puts the code under test on the near side of this executable's own
# close(2) definition.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_STATIC_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${VULKAN_VIDEO_ENCODER_INCLUDE}
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

# This test defines close(2) in the executable and counts the calls the
# library makes on one watched descriptor, forwarding every one of them to
# libc through RTLD_NEXT.
#
# The binding that matters happens at STATIC LINK TIME, not at load time:
# vulkan_video_encoder_ext.cpp.o carries `U close`, the archive is linked into
# this executable, and the executable defines close -- so the code under test
# calls this counter and nothing else.
#
# WHAT THE SHIPPED BINARY ACTUALLY SHOWS:
#
#   nm vulkan_video_encoder_ext.cpp.o | grep -w 'U close'  -> present
#   nm <exe>    | grep -w close   -> `t close`: LOCAL, defined, in .symtab
#   nm -D <exe> | grep -w close   -> nothing
#   readelf --dyn-syms <exe> | grep -w close -> nothing
#
# The third and fourth lines are the same fact stated twice, and the earlier
# claim that `readelf --dyn-syms ... $8=="close"` showed "FUNC, defined" was
# simply false: there is NO `close` in .dynsym at all, defined or undefined.
# The definition is LOCAL because the project builds with hidden visibility,
# and a local symbol is never placed in the dynamic table.
#
# `nm -D | grep -w close -> nothing` is still worth recording and is still
# true: a residual UNDEFINED `close` in .dynsym would mean some call site in
# this executable still reaches libc through the PLT. It is a NEGATIVE check
# and it is all the symbol table can offer here -- it cannot show that the
# encoder archive binds to the local definition, only that nothing in the
# executable binds past it.
#
# WHAT ACTUALLY ESTABLISHES THE BINDING is the RED/GREEN differential the
# suite produces: the same test source, linked against the unfixed library,
# reports closes=0 on all six refusals; linked against the fixed one it
# reports closes=1, cross-checked by an independent fcntl -> EBADF and a
# /proc/self/fd delta of -1. A counter the library never reached could not
# move between those two builds. Symbol-table inspection is corroboration,
# not the proof.
#
# --export-dynamic is asked for below so the same definition would be offered
# to shared libraries too. It is INERT as things stand, and measurably so: the
# symbol is LOCAL under hidden visibility, so --export-dynamic has nothing to
# export and the Vulkan driver's own fd traffic is NOT counted. That is the
# scope this test wants anyway, since every assertion here is about what the
# ENCODER LIBRARY did to one descriptor. The flag is kept because it costs
# nothing and would matter if the visibility settings ever changed; it is
# recorded as inert so nobody reads it as evidence of anything.
if(UNIX AND NOT APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--export-dynamic)
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

# CTest semantics, matching the sibling release-fence test: 0 every assertion
# held, 1 an assertion failed, 77 no encode-capable GPU and therefore nothing
# proved either way -- reported as SKIPPED, never as a pass.
enable_testing()
add_test(NAME EncoderExtAcquireFenceFdOwnership
         COMMAND ${PROJECT_NAME})
set_tests_properties(EncoderExtAcquireFenceFdOwnership PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu"
                     TIMEOUT 900)

# ---------------------------------------------------------------------------
# A SECOND ENTRY: THE ORDERING CLAIM, ON ITS OWN, AS A GATE.
# ---------------------------------------------------------------------------
# The header promises "Supplying an acquireFenceFd and a pWaitSemaphores array
# together is legal and loses neither". It holds, and this entry is what says
# so by name. --caller-wait-order runs ONLY that case, so a CTest failure line
# points at the claim rather than at the whole fd-ownership suite.
#
# It carried WILL_FAIL for one day, on the strength of a measurement that was
# an artifact. The mutation proof for the case forces the carry-across loop at
# vulkan_video_encoder_ext.cpp:6268 to zero iterations; that mutant was built,
# run red, reverted -- and the revert restored an mtime OLDER than the object
# the mutant had produced, so make rebuilt nothing and every later run re-ran
# the mutant. Rebuilding the mutant reproduces the reported red exactly,
# including its second fingerprint: a red run emits no "asyncAssemblyFence is
# not done after N mSec" warning because the frame is never parked, a green
# run emits two. Clean source is green 63 runs out of 63.
#
# So WILL_FAIL is gone, the library was not changed, and this is a gating
# assertion in both directions: the case also holds a caller TIMELINE SIGNAL
# open, which is the first coverage in this tree of the release-fence
# append's own carry-across loop.
add_test(NAME EncoderExtAcquireFdKeepsCallerWaitArray
         COMMAND ${PROJECT_NAME} --caller-wait-order)
set_tests_properties(EncoderExtAcquireFdKeepsCallerWaitArray PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_acquire_fd_test: Configured")
