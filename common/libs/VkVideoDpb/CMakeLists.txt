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

# vkvideo-dpb, the encoder's DPB library: C99, no Vulkan calls, no Vulkan objects. It is linked
# statically into the encoder library with hidden symbols, so the installed library set and the
# exported symbols do not change.
add_library(vkvideo-dpb STATIC
    src/vvs_dpb.c
    src/vvs_dpb_h264.c
    src/vvs_dpb_h265.c
    src/vvs_dpb_av1.c
    src/vvs_dpb_internal.h
    include/vkvideo_dpb/vvs_dpb.h
    include/vkvideo_dpb/vvs_dpb.hpp
)
target_include_directories(vkvideo-dpb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${VULKAN_HEADERS_INCLUDE_DIR})
set_target_properties(vkvideo-dpb PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)
if(MSVC)
    target_compile_options(vkvideo-dpb PRIVATE /W4)
else()
    target_compile_options(vkvideo-dpb PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Device-free: the library's lifecycle, slots, resources, rewind and checkpoints, and the planners'
# decisions, from C99.
if(BUILD_TESTS)
    add_executable(vkvideo-dpb-test tests/vvs_dpb_test.c)
    target_link_libraries(vkvideo-dpb-test PRIVATE vkvideo-dpb)
    set_target_properties(vkvideo-dpb-test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    install(TARGETS vkvideo-dpb-test RUNTIME DESTINATION bin)
    enable_testing()
    add_test(NAME VkVideoDpbLibrary COMMAND vkvideo-dpb-test)
    # LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
    set_tests_properties(VkVideoDpbLibrary PROPERTIES LABELS "device-free")
endif()
