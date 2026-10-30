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
project(validation_gate_fake_layer_output LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The gate (cmake/validation_gate.cmake), tested without a device: this
# program prints the loader's and the validation layer's output for one case
# and exits 0, and each test below runs it through the gate as
# vvs_add_validation_gated_test() registers a gated test.
add_executable(${PROJECT_NAME} src/main.cpp)

# WHAT THIS GATES. The gate counts every validation error: a message with a
# VUID once per message, and an error without one -- thread safety,
# UNASSIGNED-* -- once per message too, against a ceiling an EXPECT entry may
# name by its id. Warnings without a VUID are not counted, and a message whose
# body the skew allowlist matches is not.
#
# WHAT A GREEN HERE DOES NOT MEAN. The messages are canned in the format of the
# layer this tree is run with; a layer that changes its format is not covered.
enable_testing()
vvs_add_validation_gated_test(ValidationGateFailsUnassignedError
    TARGET ${PROJECT_NAME}
    ARGS   unassigned-error)
vvs_add_validation_gated_test(ValidationGateUnassignedErrorCeiling
    TARGET ${PROJECT_NAME}
    ARGS   unassigned-error
    EXPECT UNASSIGNED-Threading-MultipleThreads-Write=1)
vvs_add_validation_gated_test(ValidationGateIgnoresUnassignedWarning
    TARGET ${PROJECT_NAME}
    ARGS   unassigned-warning)
vvs_add_validation_gated_test(ValidationGateAllowlistsUnassignedBody
    TARGET ${PROJECT_NAME}
    ARGS   unassigned-allowed)
vvs_add_validation_gated_test(ValidationGateFailsVuidError
    TARGET ${PROJECT_NAME}
    ARGS   vuid-error)
vvs_add_validation_gated_test(ValidationGateCountsVuidOnce
    TARGET ${PROJECT_NAME}
    ARGS   vuid-error
    EXPECT VUID-vkCmdDispatch-None-08114=1)
# The two failing cases pass when the gate fails them for the message, and
# for nothing else: their output must name the id past its ceiling of zero.
# CMake wraps a long error at spaces, so the count may follow on a new line.
set_tests_properties(ValidationGateFailsUnassignedError PROPERTIES
                     PASS_REGULAR_EXPRESSION "UNASSIGNED-Threading-MultipleThreads-Write[ \t\n]+1/0")
set_tests_properties(ValidationGateFailsVuidError PROPERTIES
                     PASS_REGULAR_EXPRESSION "VUID-vkCmdDispatch-None-08114[ \t\n]+1/0")
# LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
set_tests_properties(ValidationGateFailsUnassignedError ValidationGateUnassignedErrorCeiling
                     ValidationGateIgnoresUnassignedWarning ValidationGateAllowlistsUnassignedBody
                     ValidationGateFailsVuidError ValidationGateCountsVuidOnce PROPERTIES
                     LABELS "device-free")

message(STATUS "validation_gate_fake_layer_output: Configured")
