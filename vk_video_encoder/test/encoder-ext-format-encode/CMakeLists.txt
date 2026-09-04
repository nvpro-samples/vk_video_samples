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

project(encoder_ext_format_encode_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The STATIC library, for the same reason the sibling taxonomy test links it:
# the internal header's free functions -- the format taxonomy and
# VkEncProbeResource, which is how this test reads a slot's resolved input
# path -- are deliberately not exported from libvkvideo-encoder.so.
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

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VK_USE_VIDEO_QUEUE
    VK_USE_VIDEO_DECODE_QUEUE
    VK_USE_VIDEO_ENCODE_QUEUE
)

# Must branch the same way the library did -- an RGBA session cannot route
# FILTER at all in a build without the filter, and a test that assumed it
# present would fail for the wrong reason.
if(BUILD_ENCODER_COMPUTE_FILTER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add test.
#
# THE NAME IS NOT EncoderExtInputFormatMatrix. This block was copied verbatim
# from encoder-ext-format-matrix next door, comment header included, so two
# different binaries were registered under one ctest name: `ctest -R
# EncoderExtInputFormatMatrix` ran both, and a name-keyed CI line was
# ambiguous between them. The CI set-comparison could not catch it either --
# it compares COUNTS, and 29 == 29 with one name appearing twice.
#
# Rows that are not abandoned by the safety guard create a real encode session
# and a real VkImage on the LIBRARY-OWNED device and then check that BITSTREAM
# BYTES CAME BACK -- and that the GPU was not lost, that no frame was dropped,
# that the submit loop ran to completion and that the output file opened. That
# is what separates this from the routing test next door: that one proves a
# descriptor registers and a slot resolves, and contains no encode call at all.
#
# AND THEN IT LOOKS AT THE PIXELS, which is the half that was missing. Every
# row that encodes is handed to ffmpeg -- a decoder outside this process -- and
# the four quadrant centres of frame 0 are compared against the primaries the
# harness wrote, to a tolerance of 24/255. That assertion is in the DEFAULT
# verdict chain, not behind a flag, because the state it catches is invisible
# to everything else here: delete the four vkCmdDispatch calls in
# VulkanFilterYuvCompute.cpp and this test still reported
# `sessions=8 encoded=8 abandoned=0 failures=0` and exit 0, with dispatch=12 on
# every filter row, while all five filter-routed rows decoded to a flat
# (0,76,0). See kQuadTolerance in src/main.cpp for the measurements.
#
# A 3-plane row that does not resolve to FILTER is ABANDONED before any submit
# (staging one is a measured GPU hang) and is neither encoded nor failed here;
# the routing test next door is what turns that regression red.
#
# So this is a gpu-label test. It exits 77 (SKIPPED) in exactly two cases, and
# 1 for any other failure:
#
#   1. every row reports VK_ERROR_INCOMPATIBLE_DRIVER, i.e. no usable driver;
#   2. ffmpeg is not on PATH, so the four-quadrant decode assertion could not
#      be rendered on the rows that encoded.
#
# The second is deliberate and is NOT a convenience. A gate that passes when
# its decoder is missing is just another test that cannot fail, and the
# counters this suite would be left with are green with the compute filter
# dead. So an unjudged run reports SKIPPED, loudly, with the reason on stdout,
# rather than PASSED. A genuine failure is still 1: main() returns 1 on
# g_failures before it ever considers the skip, so the skip cannot launder one.
#
# Everything else is 1 -- including "no session at all for a reason that is not
# an absent driver", which is a library refusal wearing an environment costume.
enable_testing()
add_test(NAME EncoderExtInputFormatEncode
         COMMAND ${PROJECT_NAME})
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(EncoderExtInputFormatEncode PROPERTIES
                     SKIP_RETURN_CODE 77
                     LABELS "gpu")

# ---------------------------------------------------------------------------
# THE FILTER-ARM RESTORE, WHICH SHIPPED WITH NO COVERAGE AT ALL.
#
# The filter branch of StageInputFrame hands the input image back with
# RestoreStagedInputLayout. For a consumer that declares GENERAL -- which
# equals that arm's residual -- the helper's equal-layout early return fires
# and no barrier is recorded, so the call does nothing.
#
# --declare-tso is the shape that exercises it: the registration declares
# TRANSFER_SRC_OPTIMAL, which differs from the filter arm's residual (GENERAL),
# so the early return does not fire and a real barrier is recorded on every
# frame of every filter-routed row that can legally hold that layout.
#
# In the default mode no restore barrier is recorded at all -- the early
# return fires on every frame -- while under --declare-tso one is recorded
# per frame of every filter-routed row (GENERAL -> TRANSFER_SRC_OPTIMAL).
#
# AND IT IS LOAD-BEARING, which a count could not have shown. Suppressing ONLY
# the CmdPipelineBarrier2KHR inside RestoreStagedInputLayout, leaving its
# return value -- so the registration's residual record still claims
# TRANSFER_SRC_OPTIMAL while the image is really still in GENERAL -- makes
# frame 2's acquire name a layout the image is not in, and the validation
# layer raises VUID-vkCmdDraw-None-09600 for every plane of every affected
# frame under --declare-tso. The default mode reports nothing, because it has
# nothing to suppress -- which is the coverage gap this mode closes.
add_test(NAME EncoderExtInputFormatEncodeDeclaredTransferSrcOptimal
         COMMAND ${PROJECT_NAME} --declare-tso)
set_tests_properties(EncoderExtInputFormatEncodeDeclaredTransferSrcOptimal
                     PROPERTIES SKIP_RETURN_CODE 77 LABELS "gpu")

# ---------------------------------------------------------------------------
# THE AV1 ARM. This is the only ctest entry that covers it.
#
# AV1 also runs inside EncoderExtInputFormatEncode above -- the four AV1 rows
# are in kRows, so the default gating entry already covers them, and a broken
# AV1 arm turns THAT red on hardware that has AV1. This second entry exists
# because a codec-scoped run reports a codec-scoped verdict: `ctest -R Av1`
# answers "AV1: passed / skipped / failed" instead of leaving the reader to
# find four rows inside thirteen.
#
# HOW IT STAYS GREEN ON THE A4000, WHICH IS THE QUESTION WORTH ANSWERING.
# The RTX A4000 has no VK_KHR_video_encode_av1, so all four rows get no
# session and this entry creates ZERO sessions. That
# combination hit main()'s `nSession == 0` branch, which returns 1 for
# anything other than "every row said VK_ERROR_INCOMPATIBLE_DRIVER" -- and
# these rows say VK_ERROR_FEATURE_NOT_PRESENT, because there IS a driver.
# Registering this entry as-is would therefore have turned A4000 CI RED,
# which is why main() now distinguishes a DEVICE refusal from a LIBRARY
# refusal (IsDeviceLimitedInit) and reports the former as 77.
#
# So on hardware without AV1 this entry is SKIPPED via the SKIP_RETURN_CODE 77
# already declared for its siblings, with the reason and the per-device
# capability probe on stdout. It is not silent, and it is not green-by-default:
# main() reaches that 77 only with g_failures == 0 AND no unverified row, and
# an AV1 row that gets no session on a device that DOES advertise the
# extension is counted as a failure before the skip is ever considered.
#
# The one thing this entry cannot do on such hardware is pass. That is
# correct: it has encoded nothing.
add_test(NAME EncoderExtInputFormatEncodeAv1
         COMMAND ${PROJECT_NAME} --codec av1)
set_tests_properties(EncoderExtInputFormatEncodeAv1
                     PROPERTIES SKIP_RETURN_CODE 77 LABELS "gpu")

# ---------------------------------------------------------------------------
# THE CONTENT PROBE'S ONLY EXECUTING TEST, ANYWHERE IN THE TREE.
#
# test/encoder-ext-import-content is device-free by construction: it drives the
# scorer and the latch directly and registers against a NULL-BACKEND session
# where no frame is ever submitted. It can therefore assert that a registration
# ARMS, and it cannot assert that an armed registration ever produces a
# VERDICT. Nothing else submitted a frame with the probe chained.
#
# That gap sat directly on top of the change that needed it most. A DIRECT
# (block-linear + VIDEO_ENCODE_SRC) registration only reaches the capture site
# because VkVideoEncoder::SetExternalInputFrameWithNode sends its FIRST frame
# down a staged detour. This entry is what executes that detour.
#
# THE GAP, DEMONSTRATED RATHER THAN ASSERTED. Reverting the detour to
# `if (directlyEncodable)` instead of
# `if (directlyEncodable && !probeStillOwesACapture)` gives:
#
#   ctest -L device-free                 6/6 passed
#   encoder_ext_import_content_test      checks: 118, failures: 0  RESULT: PASS
#   THIS ENTRY                           exit 1, row [1/13] NV12:
#     "the registration ARMED and NOTHING WAS EVER SCORED (probed=0 ...)"
#
# HEALTHY, same host, row [1/13] NV12 (DIRECT, VK_IMAGE_TILING_OPTIMAL, i.e.
# the block-linear class the periodicity harness measured poisoned):
#   armEcho=ARMED(gen=1) final=CLEAN probed=1 damaged=0 armedOutstanding=0
#   meanY=31983 meanU=32768 meanV=32768 (Q8)
#
# THOSE CHROMA NUMBERS ARE THE PROOF THE CAPTURE READ THE REAL PIXELS, not
# merely that it ran. The harness uploads a four-quadrant red/green/blue/white
# bar. In BT.709 limited range the Cb coefficients of R, G and B sum to
# exactly zero (-0.1146 - 0.3854 + 0.5), and so do the Cr coefficients
# (0.5 - 0.4542 - 0.0458), so the three colour quadrants contribute exactly
# 3*128 and the white quadrant 128: mean 128.000 in BOTH chroma channels, or
# 32768 in the Q8 units this field carries. Measured 32768 and 32768. Luma
# predicts 125.500 (Q8 32128) against a measured 31983, the 0.57 gap being
# 8-bit quadrant rounding plus the scorer's every-8th-row sampling.
#
# It is a gpu-label test and shares the 77/SKIPPED contract of its siblings.
add_test(NAME EncoderExtInputFormatEncodeContentProbe
         COMMAND ${PROJECT_NAME} --content-probe)
set_tests_properties(EncoderExtInputFormatEncodeContentProbe
                     PROPERTIES SKIP_RETURN_CODE 77 LABELS "gpu")

# THE DECODER IS A RUNTIME DEPENDENCY OF THE TEST, NOT A BUILD DEPENDENCY, and
# deliberately not a hard one at configure time. In this project the machine
# that configures and builds (no GPU) is not the machine that runs (the GPU
# host), so a find_program() failure here would say nothing about whether the
# assertion can run where it matters -- and making it REQUIRED would break a
# build that is perfectly capable of producing the binary. The binary probes
# for ffmpeg itself and reports SKIPPED if it is absent. This is a heads-up for
# whoever configures, nothing more.
find_program(FFMPEG_FOR_ENCODE_GATE ffmpeg)
if(NOT FFMPEG_FOR_ENCODE_GATE)
    message(STATUS "encoder_ext_format_encode_test: ffmpeg not found on the "
                   "BUILD host. The four-quadrant decode assertion needs it on "
                   "the RUN host; without it there, this test reports SKIPPED "
                   "rather than PASSED.")
else()
    message(STATUS "encoder_ext_format_encode_test: decode gate will use "
                   "${FFMPEG_FOR_ENCODE_GATE} if the run host has it")
endif()

message(STATUS "encoder_ext_format_encode_test: Configured")
