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

project(encoder_ext_adopt_device_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.cpp)

# The PUBLIC API only, like the sibling real-device tests. The whole point of
# this one is that an EMBEDDER can drive the library through the shipped
# header; reaching an internal seam would weaken exactly the claim it makes.
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
# VALIDATION GATING.
#
# Every entry below runs through validation_gate.cmake rather than calling the
# binary directly, because the binary CANNOT REPORT A VALIDATION FAILURE ITSELF.
# On the --own-validate arm the LIBRARY creates the VkInstance, so the harness
# has nothing to attach a VkDebugUtilsMessenger to, and the ext API exposes no
# message callback -- vulkan_video_encoder_ext.h's `VkBool32 validate` is a bare
# bool. Measured consequence: the layer reports 46 errors while the binary
# prints "PASSED : 5 checks, 0 failures" and exits 0.
#
# WHAT PINS WHAT. VK_LAYER_SETTINGS_PATH controls HOW MANY messages print;
# VK_LAYER_PATH controls WHETHER ANY DO. Only the second decides whether this
# suite can fail at all.
#
# READ THE DEFAULT HONESTLY: VVS_VALIDATION_LAYER_PATH is EMPTY unless set, and
# while it is empty VK_LAYER_PATH is NOT placed in the tests' ENVIRONMENT -- the
# ambient shell decides, and the validation arms skip on any machine that has
# not exported it. That is the configuration this ships in, and it is why these
# arms can still be green over a live defect. Set VVS_VALIDATION_LAYER_PATH to
# make a build self-contained, and VVS_REQUIRE_VALIDATION_LAYER=ON so a fleet
# cannot sit green purely because the layer was missing everywhere.
#
# SCOPE: this gates the 7 tests in THIS file. Other GPU tests in the tree are
# ungated and VVS_REQUIRE_VALIDATION_LAYER does not reach them.
# --------------------------------------------------------------------------
# Matched against the MESSAGE BODY, not the VUID name -- the name cannot tell
# layer/header skew ("unknown VkStructureType") from a struct that is genuinely
# not permitted in that chain ("which is not allowed here"), and both arrive
# under VUID-Vk<Any>-pNext-pNext.
set(VVS_VALIDATION_ALLOW_VUIDS "unknown VkStructureType" CACHE STRING
    "Regex matched against a validation message BODY; matches are tolerated")
option(VVS_REQUIRE_VALIDATION_LAYER
       "Fail, rather than skip, the validation arms when no layer is installed"
       OFF)
set(VVS_VALIDATION_LAYER_PATH "" CACHE PATH
    "Directory holding a validation-layer manifest; becomes the tests' VK_LAYER_PATH")
set(VVS_VALIDATION_LAYER_LIBDIR "" CACHE PATH
    "Directory added to the tests' LD_LIBRARY_PATH. The Vulkan SDK's layer manifest names a bare soname, so without this it loads only if the library is already on the default search path -- the reason an earlier --own-validate run skipped despite VK_LAYER_PATH being set.")

set(_vvs_test_env
    "VK_LAYER_SETTINGS_PATH=${CMAKE_CURRENT_BINARY_DIR}/vk_layer_settings.txt")
if(VVS_VALIDATION_LAYER_PATH)
  list(APPEND _vvs_test_env "VK_LAYER_PATH=${VVS_VALIDATION_LAYER_PATH}")
endif()
if(VVS_VALIDATION_LAYER_LIBDIR)
  # Replaces rather than prepends. Deliberate: it is opt-in, and a test whose
  # loader path half-comes-from-the-ambient-shell is not reproducible.
  list(APPEND _vvs_test_env "LD_LIBRARY_PATH=${VVS_VALIDATION_LAYER_LIBDIR}")
endif()

# SKIP_REGULAR_EXPRESSION, not SKIP_RETURN_CODE. Measured on ctest 3.28.3:
# SKIP_RETURN_CODE beats FAIL_REGULAR_EXPRESSION, so a run that emitted
# validation errors on its way to a 77 exit was recorded as Skipped rather than
# Failed -- and main.cpp has two 77 returns that are reached after the layer is
# live. The gate script emits VALIDATION_GATE_RESULT=SKIP only on a path that
# has already established there were no validation errors, so the precedence
# cannot bite: the token does not exist on any failing path.
function(vvs_add_gated_test _name)
  add_test(NAME ${_name}
           COMMAND ${CMAKE_COMMAND}
                   -DTEST_EXE=$<TARGET_FILE:${PROJECT_NAME}>
                   "-DTEST_ARGS=${ARGN}"
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
# THE SUBJECT. An ADOPT-mode session: the embedder owns the VkInstance and
# names the VkPhysicalDevice, the library creates its own VkDevice on them and
# encodes. This is the recommended embedding shape, and until this file
# existed no test in the tree touched externalInstance /
# externalPhysicalDevice / externalDevice at all.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptInstanceAndPhysicalDevice)

# ---------------------------------------------------------------------------
# THE CONTROL, and it is what makes a red subject attributable. Identical
# config with NO external handles, i.e. the OWN path that ships today. If the
# subject is red and this is green, the host encodes fine and the difference
# is the adopted instance / physical device. If both are red, the machine is
# the story.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptOwnControl --own)

# ---------------------------------------------------------------------------
# THE PIN. An adopted physical device PLUS a deviceId that names a different
# GPU. The library must refuse rather than quietly select something else --
# PLAN:427 makes physical-device pinning REQUIRED once a device is named,
# and on a single-GPU host a refusal is the only observable that separates a
# real pin from an advisory one.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptPhysicalDevicePinIsEnforced --conflict)

# ---------------------------------------------------------------------------
# THE CONTEXT PATH -- the same adoption expressed the way the context design
# requires. The context design splits the library's bring-up as "context =
# phases 1+2, session = phases 3+4", and the header's context rule 4 says a
# context creates no VkDevice. This case proves the split works end to end: an
# ADOPT context over the embedder's instance and physical device, a session
# created ON it with a config that names NO external handles at all, and a real
# encode. The instance/physical-device assertions further down the file are
# shared with the config-level ADOPT case, so a pass here means the context
# path borrowed exactly what the config path borrows.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptViaContext --context)

# ---------------------------------------------------------------------------
# THE COLLISION. A session on a context AND a config that still names
# externalInstance/externalPhysicalDevice. The two express the same borrowing,
# so honouring either one silently is the failure: a stale config field
# outranking the context would send the session to a different physical device
# than the one whose capability snapshot the caller read, with nothing
# downstream reporting the substitution. It must be a typed refusal.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtContextRefusesConflictingConfigHandles --context-conflict)


# ---------------------------------------------------------------------------
# VALIDATION OVER A BORROWED INSTANCE. config.validate on an ADOPTED instance.
# The library never created that instance, so it cannot have enabled the
# validation layer or the debug-callback extension on it; it must check what
# the instance ACTUALLY has rather than what the loader offers. The embedder
# here enables the validation layer but deliberately NOT VK_EXT_debug_report,
# which is the realistic compositor shape.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptValidateOverBorrowedInstance --validate)

# ---------------------------------------------------------------------------
# THE NEGATIVE CONTROL FOR THE ARM ABOVE. config.validate with the library
# owning the instance. The refusal to attach a debug callback is supposed to
# be scoped to IMPORTED instances; a change that stopped attaching callbacks
# altogether would turn the --validate arm green and could not be told apart
# from the real fix without this entry.
#
# THIS IS THE ONE ENTRY IN THE SUITE THAT NEEDS AN INSTALLED VALIDATION LAYER,
# because here the LIBRARY creates the instance and asks for
# VK_LAYER_KHRONOS_validation by name. Without it on the loader's search path
# the session cannot come up at all and the entry SKIPS (77) rather than
# pretending to prove something. Export VK_LAYER_PATH to a directory holding
# the layer manifest to make it run. The SUBJECT arm above deliberately does
# NOT need this -- it was rewritten to use a bare embedder instance precisely
# so it could not skip on the machine it guards.
# ---------------------------------------------------------------------------
vvs_add_gated_test(EncoderExtAdoptValidateOwnInstanceControl --own-validate)

message(STATUS "encoder_ext_adopt_device_test: Configured")
