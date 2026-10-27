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
project(vk-video-parser-trace LANGUAGES CXX)

# The parser is linked from its static archive: the tools need no Vulkan loader, no device and no
# shared library, which is what lets the tests carry the device-free label.
add_executable(${PROJECT_NAME} src/ParserTrace.cpp src/StreamInput.cpp src/TraceClient.cpp)
add_executable(vk-video-parser-crafted src/ParserCrafted.cpp src/TraceClient.cpp)
# The parser fuzz target without libFuzzer: it runs fuzz inputs from files, and the seed inputs it
# derives from streams, with any compiler.
add_executable(vk-video-parser-fuzz-replay src/ParserFuzz.cpp src/StreamInput.cpp src/TraceClient.cpp)
# The decoder sample's parser adapter, compiled in: it makes no Vulkan call, so it runs with no
# loader and no device as the parser does.
add_executable(vk-video-adapter-trace src/AdapterTrace.cpp src/StreamInput.cpp src/TraceClient.cpp
               ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoParser/VulkanVideoParser.cpp)
target_compile_definitions(vk-video-adapter-trace PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vk-video-adapter-trace PRIVATE ${VULKAN_VIDEO_APIS_INCLUDE})

# The libFuzzer targets fuzz_parser_<codec>, for clang. Configure the build so its flags instrument
# the parser for the fuzzer and the sanitizers, without assertions, which stop at the parser's
# asserts on bitstream content (README.md, Fuzzing):
#   CC=clang CXX=clang++ cmake -DBUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#       -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" \
#       -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" ...
option(BUILD_FUZZERS "Build the libFuzzer parser targets fuzz_parser_<codec> (clang)" OFF)
set(PARSER_FUZZ_TARGETS)
if(BUILD_FUZZERS)
    foreach(codec h264 h265 av1 vp9)
        add_executable(fuzz_parser_${codec} src/ParserFuzz.cpp src/StreamInput.cpp src/TraceClient.cpp)
        target_compile_definitions(fuzz_parser_${codec} PRIVATE PARSER_FUZZ_CODEC="${codec}")
        target_link_options(fuzz_parser_${codec} PRIVATE -fsanitize=fuzzer)
        list(APPEND PARSER_FUZZ_TARGETS fuzz_parser_${codec})
    endforeach()
endif()

foreach(tool ${PROJECT_NAME} vk-video-parser-crafted vk-video-adapter-trace vk-video-parser-fuzz-replay ${PARSER_FUZZ_TARGETS})
    target_include_directories(${tool} PRIVATE
        ${VULKAN_VIDEO_PARSER_INCLUDE}/..
        ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
        ${VULKAN_HEADERS_INCLUDE_DIR}
    )
    target_link_libraries(${tool} PRIVATE ${VULKAN_VIDEO_PARSER_STATIC_LIB})
endforeach()

install(TARGETS ${PROJECT_NAME} vk-video-parser-crafted vk-video-adapter-trace vk-video-parser-fuzz-replay RUNTIME DESTINATION bin)

# WHAT THIS GATES. Every stream in suites/<codec>.json that is available runs through the parser,
# and these checks apply:
# - golden: the trace equals the recorded golden, so any change in what the parser hands a decoder
#   fails until the golden is re-recorded with that change explained. Streams outside the tree
#   (the decode samples, conformance corpora) are held to the md5 of their trace instead;
# - model: a model compares the trace with what the specification makes of the stream's syntax
#   elements: scripts/av1_reference_model.py replays the AV1 reference state (AV1 5.9.2, 7.8,
#   7.20, 7.21); scripts/h264_header_model.py, h265_header_model.py and vp9_header_model.py check
#   the parameter sets, the POC and reference picture sets, and the VP9 frame headers. Each stream
#   lists the mismatch categories of known defects; a category that appears unlisted fails, and a
#   listed one that no longer appears fails too, so a fix has to remove its entry.
#
# The committed streams under streams/ carry their header dumps, so no check needs ffmpeg.
# Streams under an environment-variable root run only when that variable is set.
#
# WHAT A GREEN HERE DOES NOT MEAN. Nothing is decoded. The test shows what the parser hands a
# client; whether a driver decodes it correctly is the GPU suites' question.
enable_testing()
foreach(codec Av1 H264 H265 Vp9)
    string(TOLOWER ${codec} suite)
    add_test(NAME ParserTrace${codec}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/parser_trace_suite.py
                     --tool $<TARGET_FILE:${PROJECT_NAME}>
                     --suite ${CMAKE_CURRENT_SOURCE_DIR}/suites/${suite}.json)
    # LABELS: this is the CI gating set. See the top-level CMakeLists.txt note.
    set_tests_properties(ParserTrace${codec} PROPERTIES LABELS "device-free")
endforeach()

# The adapter slot trace: vk-video-adapter-trace runs the decoder sample's parser adapter over
# the same suites, and scripts/slot_oracle.py checks each decode's DPB slots, Std reference
# information and begin-coding list against the DPB slot rules of the Vulkan specification. The
# trace equals the stream's adapter_golden (or its md5 the set's adapter_md5 list), and the oracle's
# categories equal adapter_known, each one a defect the suite's adapter_defects table names. No
# check needs ffmpeg.
foreach(codec Av1 H264 H265 Vp9)
    string(TOLOWER ${codec} suite)
    add_test(NAME AdapterTrace${codec}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/parser_trace_suite.py --mode adapter
                     --tool $<TARGET_FILE:vk-video-adapter-trace>
                     --suite ${CMAKE_CURRENT_SOURCE_DIR}/suites/${suite}.json)
    set_tests_properties(AdapterTrace${codec} PROPERTIES LABELS "device-free")
endforeach()

# The fuzz replay: each codec's committed streams, cut into packets in every mode of the fuzz
# input, and the inputs under fuzz/regressions/<codec>, through the fuzz target. A pass is a run
# that neither crashes nor hangs; in a sanitizer build (scripts/run_ctest_sanitizers.sh), one the
# sanitizers report nothing in.
foreach(codec Av1 H264 H265 Vp9)
    string(TOLOWER ${codec} name)
    if((name STREQUAL "h264") OR (name STREQUAL "h265"))
        set(format bytestream)
        set(extension ${name})
    else()
        set(format ivf)
        set(extension ivf)
    endif()
    file(GLOB fuzz_streams CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/streams/${name}-*.${extension})
    file(GLOB fuzz_regressions CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${name}/*)
    set(fuzz_args)
    foreach(stream ${fuzz_streams})
        list(APPEND fuzz_args -i ${stream})
    endforeach()
    add_test(NAME ParserFuzzReplay${codec}
             COMMAND $<TARGET_FILE:vk-video-parser-fuzz-replay> --codec ${name} --format ${format} ${fuzz_args}
                     ${fuzz_regressions})
    set_tests_properties(ParserFuzzReplay${codec} PROPERTIES LABELS "device-free" TIMEOUT 60)
endforeach()

message(STATUS "vk-video-parser-trace: Configured")

# The crafted cases: streams built in src/ParserCrafted.cpp, each reaching one parser path, with
# checks on what the parser hands its client. A case that hangs the parser fails at the timeout,
# and one in a sanitizer build fails on any report the sanitizer makes fatal. ParserCraftedCases
# fails unless this list names exactly the cases the tool has.
set(PARSER_CRAFTED_CASES
    H264CraftedStream
    H264PpsSliceGroupMapUnits
    H265CraftedStream
    H265VpsLayerSetCountWrap
    H265VpsLayerSetCountLimit
    H265VpsExtensionLayerSetIndex
    H265InterRpsOverflow
    H265SpsNumStRps
    Av1CraftedSequenceHeader
    Av1UvlcTruncated
    Av1FilmGrainPointCount
    Vp9CraftedKeyFrame
)
foreach(crafted ${PARSER_CRAFTED_CASES})
    add_test(NAME ParserCrafted.${crafted} COMMAND $<TARGET_FILE:vk-video-parser-crafted> --case ${crafted})
    set_tests_properties(ParserCrafted.${crafted} PROPERTIES LABELS "device-free" TIMEOUT 10)
endforeach()
add_test(NAME ParserCraftedCases
         COMMAND $<TARGET_FILE:vk-video-parser-crafted> --expect-cases ${PARSER_CRAFTED_CASES})
set_tests_properties(ParserCraftedCases PROPERTIES LABELS "device-free")
