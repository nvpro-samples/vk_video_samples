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

project(encoder_ext_drain_assembly_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The PUBLIC API only, like the sibling release-fence and acquire-fd tests --
# a real device needs no internal seams.
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

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

enable_testing()

# ---------------------------------------------------------------------------
# WAS A KNOWN FAILURE. FIXED -- WILL_FAIL REMOVED.
# ---------------------------------------------------------------------------
# This test used to exit 1 and carried WILL_FAIL TRUE. It pinned the defect
# described at the top of src/main.cpp: DrainPendingFrames() ->
# WaitForThreadsToComplete() set m_asyncAssemblyEnabled = false and joined the
# only threads that ever call PushCapturedBitstream, and nothing outside
# InitEncoder ever turned it back on -- so no frame submitted after that call
# could produce a completion record, and every one of them held its
# PendingFrame and its resource registration forever.
#
# The fix is VkVideoEncoder::DrainAndRestartThreads(), which DrainPendingFrames
# now calls: it performs the same drain and then restarts the assembly
# workers, so the completion surface survives a drain. The binary exits 0, and
# WILL_FAIL is gone -- it is a plain gating test again. Leaving WILL_FAIL in
# place would have reported the working fix as a FAILURE, which is precisely
# what it was there to force.
#
# Exit 77 on a host with no encode-capable GPU is still reported as SKIPPED:
# the gate recognises that code and emits the skip token CTest matches on.
#
# GATED ON VALIDATION. The drain restarts the assembly workers mid-session and
# the frames submitted after it re-enter the same registration and submission
# path, which is where a lifetime or synchronisation violation would appear.
# vvs_add_validation_gated_test() forces the validation layer on and fails
# the arm that produced a message; no ceiling is declared, so any message at
# all is a failure.
vvs_add_validation_gated_test(EncoderExtDrainPendingFramesIsNotTerminal
    TARGET ${PROJECT_NAME})
set_tests_properties(EncoderExtDrainPendingFramesIsNotTerminal PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

# The SECOND arm -- and it is not optional.
#
# The defect above is a contract violation in BOTH output modes: the single
# completion publisher lives inside WriteBitstreamToFile, which serves the
# capture arm and the file-output arm alike, and DrainPendingFrames() used to
# stop the workers that call it either way. src/main.cpp says as much, and
# says it "therefore runs in file-output mode too, under --file-output, to
# show the divergence" -- but only the capture arm was ever registered, so
# half of the contract the test exists to pin was gated by nothing and had to
# be re-checked by hand every time.
#
# Same label, same gate, same timeout as its sibling, so this Skips rather
# than fails on a host with no encode-capable GPU. The two arms share no state
# -- only this one writes encoder_ext_drain_assembly_out.264 -- so they are
# safe to run in parallel.
vvs_add_validation_gated_test(EncoderExtDrainPendingFramesIsNotTerminalFileOutput
    TARGET ${PROJECT_NAME}
    ARGS   --file-output)
set_tests_properties(EncoderExtDrainPendingFramesIsNotTerminalFileOutput PROPERTIES
                     LABELS "gpu"
                     TIMEOUT 900)

message(STATUS "encoder_ext_drain_assembly_test: Configured")
