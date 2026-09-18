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

project(encoder_ext_input_residency_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The PUBLIC API only, like the sibling real-device tests. This suite's whole
# claim is about what an EMBEDDER can observe through the shipped header --
# reaching an internal seam would let it assert on a fact no caller can see,
# which is the failure mode it exists to close.
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

configure_file(vk_layer_settings.txt
               ${CMAKE_CURRENT_BINARY_DIR}/vk_layer_settings.txt COPYONLY)

# --------------------------------------------------------------------------
# VALIDATION GATING. Same wrapper, same cache variables and same rationale as
# encoder-ext-adopt-device; validation_gate.cmake and vk_layer_settings.txt
# are copied verbatim from that suite (both are self-contained).
#
# WHY THIS IS A NEW BINARY AND NOT A SIXTH ARM ON encoder-ext-adopt-device:
#
#  1. ATTRIBUTION. That suite's subject is DEVICE ADOPTION and its own CMake
#     argues that the control/subject split is what makes a red arm
#     attributable. A residency arm inside it would be red for a reason the
#     suite's name denies.
#  2. THE PINNED BASELINE. encoder_ext_adopt_device_test --own-validate is the
#     number the CF-02 work is pinned to (46 -> 0). New arms in the same
#     binary invite conflation of counts.
#  3. DIFFERENT FLOOR. This binary's validation floor is NOT zero and cannot
#     be -- see below. Mixing it into a suite whose floor IS zero would force
#     one of the two numbers to be explained away.
#
# THE FLOOR IS ZERO, AND THAT IS NOT WHAT THE REASONING BELOW PREDICTS.
#
# This suite was designed expecting a non-zero validation floor. The reasoning
# was sound as far as it went: on an OS-handle registration frame 1's acquire
# names a layout the image is not in, because the library creates the imported
# VkImage with initialLayout = UNDEFINED (forced by
# VUID-VkImageCreateInfo-pNext-01443), performs NO transition at registration
# (the public header: the declaration "must ALREADY be true when the first
# frame is submitted"), and then overwrites the one truthful declaration in
# VkVideoEncoder.cpp's `srcOldLayout = VK_IMAGE_LAYOUT_GENERAL; // Fallback
# for compute output`. The prediction was 2 messages per arm, one per NV12
# plane.
#
# The prediction is wrong: the layer reports nothing on any of the five
# arms, whether or not the residency declaration is honoured.
#
# THAT ZERO IS NOT A BLIND HARNESS. A temporary probe arm declaring
# VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL against a TRANSFER_SRC-only image
# raises VUID-VkImageMemoryBarrier2-oldLayout-01213 out of
# vkCmdPipelineBarrier2KHR on the library's own device, captured in this
# harness's own output. So the layer is loaded, it inspects this workload's
# barriers, and its messages reach the gate.
#
# WHAT THAT PROBE DOES NOT ESTABLISH, stated so the zero is not over-read: it
# fired a USAGE-compatibility VUID, not the layout-TRACKING VUID (01197). So
# "0 messages" is what this layer build reports; it is NOT proof that a
# declared-vs-actual layout mismatch would be reported here. Do not convert
# this zero into "the declarations are all true" -- see the mirror arm below,
# where the declaration is measurably false on frames 2..8 and still reports
# nothing.
#
# NOTHING IS ALLOWLISTED. VVS_VALIDATION_ALLOW_VUIDS keeps its inherited
# default and the frame-1 VUID is deliberately absent from it: allowlisting a
# real defect to make a number look better is the one thing this project's
# rules forbid outright. The suite is carried by the COUNTER assertions, which
# need no layer at all -- which is the point, since the property under test is
# invisible to the layer either way.
# --------------------------------------------------------------------------
set(VVS_VALIDATION_ALLOW_VUIDS "unknown VkStructureType" CACHE STRING
    "Regex matched against a validation message BODY; matches are tolerated")
option(VVS_REQUIRE_VALIDATION_LAYER
       "Fail, rather than skip, the validation arms when no layer is installed"
       OFF)
set(VVS_VALIDATION_LAYER_PATH "" CACHE PATH
    "Directory holding a validation-layer manifest; becomes the tests' VK_LAYER_PATH")
set(VVS_VALIDATION_LAYER_LIBDIR "" CACHE PATH
    "Directory added to the tests' LD_LIBRARY_PATH.")

set(_vvs_test_env
    "VK_LAYER_SETTINGS_PATH=${CMAKE_CURRENT_BINARY_DIR}/vk_layer_settings.txt")
if(VVS_VALIDATION_LAYER_PATH)
  list(APPEND _vvs_test_env "VK_LAYER_PATH=${VVS_VALIDATION_LAYER_PATH}")
endif()
if(VVS_VALIDATION_LAYER_LIBDIR)
  list(APPEND _vvs_test_env "LD_LIBRARY_PATH=${VVS_VALIDATION_LAYER_LIBDIR}")
endif()

# The COUNTER entries. These are the suite's load-bearing tests and they need
# no validation layer, which is deliberate: the property under test is
# invisible to the layer (both barrier programs are spec-clean), so tying it
# to a layer would make it skip on exactly the fleet it guards.
function(vvs_add_counter_test _name)
  add_test(NAME ${_name} COMMAND ${PROJECT_NAME} ${ARGN})
  set_tests_properties(${_name} PROPERTIES
                       LABELS "gpu"
                       TIMEOUT 900
                       SKIP_RETURN_CODE 77)
endfunction()

function(vvs_add_gated_test _name)
  # SPACE-separated, not ${ARGN} directly. validation_gate.cmake documents
  # TEST_ARGS as "ONE argument or a space-separated string" and splits it with
  # separate_arguments(NATIVE_COMMAND), which splits on WHITESPACE. ${ARGN} is
  # a CMake list, so it interpolates SEMICOLON-separated, and add_test records
  # the whole thing as one argv token: "-DTEST_ARGS=--arm;--validate". With no
  # whitespace in it the split was a no-op and the test binary got a single
  # unrecognised argument, printed its usage and exited 1 -- so every gated
  # test taking TWO arguments never executed a line of its subject. That was
  # both of them, and they had been red long enough to be filed as expected.
  string(JOIN " " _gated_args ${ARGN})
  add_test(NAME ${_name}
           COMMAND ${CMAKE_COMMAND}
                   -DTEST_EXE=$<TARGET_FILE:${PROJECT_NAME}>
                   "-DTEST_ARGS=${_gated_args}"
                   -DREQUIRE_LAYER=${VVS_REQUIRE_VALIDATION_LAYER}
                   "-DALLOW_VUIDS=${VVS_VALIDATION_ALLOW_VUIDS}"
                   "-DGATE_SUMMARY=${CMAKE_BINARY_DIR}/validation_gate_summary.txt"
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/validation_gate.cmake)
  set_tests_properties(${_name} PROPERTIES
                       LABELS "gpu"
                       TIMEOUT 900
                       SKIP_REGULAR_EXPRESSION "VALIDATION_GATE_RESULT=SKIP"
                       ENVIRONMENT "${_vvs_test_env}")
endfunction()

# ---------------------------------------------------------------------------
# THE SUBJECT. An OPAQUE_FD registration -- self-exported from the library's
# own VkDevice and re-imported into it, which is exactly Chromium's shipping
# CPU staging tier -- declaring VK_VIDEO_ENCODER_INPUT_RESIDENCY_LOCAL.
# The declaration must be HONOURED: no queue-family acquire, and the local
# HOST|TRANSFER availability barrier instead, on all 8 reused frames.
#
# THIS IS THE ARM THAT WAS RED. Before the residency fix it reported
# foreignAcquireCount = 8 / localAcquireCount = 0.
# ---------------------------------------------------------------------------
vvs_add_counter_test(EncoderExtInputResidencyLocalHonouredOnOpaqueFd
                     --local-opaque-fd)

# ---------------------------------------------------------------------------
# THE CONTROL, and it is what makes a red subject attributable. The IDENTICAL
# VkImage -- same create info, same exportable dedicated allocation, same
# persistent mapping, same 8-frame reuse -- registered as VK_IMAGE, the one
# handle type where residency was already honoured. Subject-red with this
# green isolates handleType and nothing else: not the host, not the format,
# not LINEAR tiling, not reuse, not the self-import.
# ---------------------------------------------------------------------------
vvs_add_counter_test(EncoderExtInputResidencyLocalControlOnVkImage
                     --local-vk-image)

# ---------------------------------------------------------------------------
# THE NEGATIVE CONTROL. An explicit RESIDENCY_FOREIGN on the same OPAQUE_FD
# shape must STILL take the queue-family acquire. Without this entry the fix
# could degenerate into "stop taking the foreign acquire", which would turn
# the subject green while silently breaking every genuine import.
# ---------------------------------------------------------------------------
vvs_add_counter_test(EncoderExtInputResidencyForeignStillAcquires
                     --foreign-opaque-fd)

# ---------------------------------------------------------------------------
# THE PIN, and it guards two live consumers by name. residency left ZERO
# (AUTO) on an OS handle must still DERIVE FOREIGN. The Wayland/GBM zero-copy
# lane is host-written and correctly FOREIGN -- its exporter is GBM/DRM -- and
# the out-of-tree Vulkan renderer app never declares residency at all. Both
# inherit AUTO, and a fix that generalised to "honour LOCAL, and while we are
# here stop deriving FOREIGN" would break both without this entry noticing.
# ---------------------------------------------------------------------------
vvs_add_counter_test(EncoderExtInputResidencyAutoStillDerivesForeign
                     --auto-opaque-fd)

# ---------------------------------------------------------------------------
# THE CHROMIUM MIRROR. This arm declares VK_IMAGE_LAYOUT_PREINITIALIZED as
# both the registration default AND the per-frame currentLayout, which is what
# media/gpu/vulkan/vulkan_video_encode_accelerator.cc did before the UNDEFINED
# sentinel change -- OPAQUE_FD, RESIDENCY_LOCAL, one reused host-written
# staging image.
#
# IT PASSES, AND IT WAS EXPECTED TO FAIL. The design registered it WILL_FAIL
# on the reasoning below; the hardware said otherwise and the registration was
# corrected rather than the measurement explained away.
#
# THE REASONING, WHICH IS STILL CORRECT ABOUT THE CODE: a non-UNDEFINED
# per-frame currentLayout is an explicit statement of fact that outranks the
# library's own record, so the residual-layout mechanism is bypassed on every
# frame and the caller re-declares PREINITIALIZED each time -- while the
# library's own handback left the image in GENERAL, because PREINITIALIZED is
# not a legal barrier destination
# (VUID-VkImageMemoryBarrier2-newLayout-01198) and RestoreStagedInputLayout
# substitutes GENERAL and records it. From frame 2 on, the declaration is
# FALSE. That is what VUID-VkImageMemoryBarrier2-oldLayout-01197 forbids.
#
# THE MEASUREMENT: 0 validation messages, exit 0, on this host and layer. So
# the declaration is false and the layer does not say so. Both halves of that
# sentence matter -- it is precisely why this defect class keeps surviving
# review, and it is why this suite's real assertions are counters.
#
# SO WHAT IS THIS ENTRY FOR. Two things, neither of them "it is red":
#   * it pins that the pre-sentinel Chromium shape still REGISTERS, encodes 8
#     reused frames and routes LOCAL, so the library half of the fix did not
#     break the caller it was aimed at before that caller has moved;
#   * it is the standing record that a green layer run over this shape does
#     NOT mean the declaration is true. Anyone tempted to keep declaring
#     PREINITIALIZED because "validation is clean" should read this block
#     first.
# The caller-side fix is the UNDEFINED sentinel, which is the truthful answer:
# the layout being described belongs to the LIBRARY's own imported VkImage,
# created initialLayout = UNDEFINED, and the caller genuinely never knew it.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtInputResidencyPreinitializedChromiumMirror
                   --preinit-opaque-fd --validate)

# ---------------------------------------------------------------------------
# THE LAYOUT-TABLE HOLE. OPAQUE_FD + RESIDENCY_LOCAL declaring
# VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL as the registration default.
#
# THIS ARM DID NOT FAIL BEFORE THE FIX -- IT ABORTED. The staging copy's
# source acquire transitions TO TRANSFER_SRC_OPTIMAL, so a caller declaring
# TRANSFER_SRC_OPTIMAL produces the pair
# (TRANSFER_SRC_OPTIMAL -> TRANSFER_SRC_OPTIMAL), which the transition table
# had no arm for. The table's terminal else is `throw std::invalid_argument`
# under __cpp_exceptions, this standalone CMake build defines it, and nothing
# on StageInputFrame's call stack catches -- so the result was
# std::terminate/SIGABRT, not a test failure. In Chromium's -fno-exceptions
# build the same pair is instead a SILENT fall-through that keeps the barrier
# struct's VIDEO_ENCODE defaults in front of a TRANSFER read.
#
# Nothing about this declaration is exotic: it is what a caller reusing a
# staging image it last used as a copy source would naturally state, it is
# what the ext layer's own legacy wrap uses as its default, and
# VkVideoEncoder.cpp:1290-1294 and :1513-1517 both promise in as many words
# that such a caller round-trips.
# ---------------------------------------------------------------------------
vvs_add_counter_test(EncoderExtInputResidencyTransferSrcOptimalDeclaration
                     --local-tso-opaque-fd)

# The same arm under the layer. The counter entry above proves the process
# survives; this one proves the barrier it now records is spec-clean --
# specifically that the arm's stage masks are legal on the family this batch
# is submitted to (VUID-vkCmdPipelineBarrier2-srcStageMask-09675) and that
# the handback leaves the image where frame 2's acquire says it is
# (VUID-VkImageMemoryBarrier2-oldLayout-01197). Neither is visible to a
# counter.
vvs_add_gated_test(EncoderExtInputResidencyTransferSrcOptimalValidated
                   --local-tso-opaque-fd --validate)

message(STATUS "encoder_ext_input_residency_test: Configured")
