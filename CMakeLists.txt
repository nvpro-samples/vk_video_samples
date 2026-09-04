cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
endif()
project(vulkan_video_codec LANGUAGES CXX C)

# Set installation paths explicitly
set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT TRUE)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install/${CMAKE_BUILD_TYPE}" CACHE PATH "Installation prefix" FORCE)
endif()

# Common includes and options
include(CheckCXXCompilerFlag)
include(FetchContent)

# Basic settings
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "Minimum OS X deployment version")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# set (CMAKE_VERBOSE_MAKEFILE 1)

# Set module path for Find*.cmake files
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

# vvs_add_validation_gated_test(), which registers a test whose
# validation-layer output is gating. Included here rather than per directory
# so that every subdirectory reaches one definition and one gate script.
include(VvsValidationGate)

# CTest aggregation. Every add_test() in this tree lives in a subdirectory,
# and CMake writes a CTestTestfile.cmake at the build ROOT only when testing is
# enabled HERE -- so without this line `cd build && ctest` reports
# "Total Tests: 0" and exits 0 having run nothing, which is a CI step that can
# never fail. The subdirectories' own enable_testing() calls make them
# runnable when configured standalone; this one makes them runnable from the
# top, which is how CI configures the project.
#
# LABEL CONTRACT -- every add_test() in this tree MUST carry exactly one of:
#
#   device-free  Needs no Vulkan device. Runs and must pass on any host,
#                including the GPU-less GitHub runner. This is the gating set.
#   gpu          Needs a real GPU. MUST also declare how it skips when no
#                device is available, so that a GPU-less host reports SKIPPED
#                and never FAILED: SKIP_RETURN_CODE for a plain add_test(),
#                or vvs_add_validation_gated_test(), which declares the
#                equivalent SKIP_REGULAR_EXPRESSION itself. A CI job red on
#                arrival is a job people learn to ignore.
#
# scripts/run_ctest_ci.sh enforces the contract: it fails if any registered
# test is in neither set, and CI calls it. Add a test, add its label.
enable_testing()

# Common options
option(BUILD_VIDEO_PARSER "Build the video parser" ON)
option(BUILD_DECODER "Build the video decoder" ON)
option(BUILD_ENCODER "Build the video encoder" ON)
option(USE_CCACHE "Use ccache" OFF)
option(BUILD_LOADER "Build loader" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_DEMOS "Build demos" ON)

# Handle ccache
if(USE_CCACHE)
    find_program(CCACHE_FOUND ccache)
    if(CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
    endif()
endif()

# Required packages
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# API settings
set(API_NAME "Vulkan" CACHE STRING "API name to use when building")
set(MAJOR "1")
string(TOLOWER ${API_NAME} API_LOWERCASE)

# Include common modules
include(FindVulkanAndDependencies)

############ VULKAN_FFMPEG_LIB_PATH ######################################
if (DEFINED ENV{VULKAN_FFMPEG_LIB_DIR_PATH})
    MESSAGE(STATUS "VULKAN_FFMPEG_LIB_DIR_PATH ENV VAR is set to $ENV{VULKAN_FFMPEG_LIB_DIR_PATH}")
    set(VULKAN_FFMPEG_LIB_PATH  "$ENV{VULKAN_FFMPEG_LIB_DIR_PATH}" CACHE PATH "Path to FFMPEG library directory" FORCE)
else()
    set(VULKAN_FFMPEG_LIB_PATH  "${VULKAN_FFMPEG_LIB_PATH}" CACHE PATH "Path to FFMPEG library directory")
endif()

if (EXISTS "${VULKAN_FFMPEG_LIB_PATH}")
    MESSAGE(STATUS "VULKAN_FFMPEG_LIB_PATH is set and valid ${VULKAN_FFMPEG_LIB_PATH}")
else()
    if(WIN32)
        if ((CMAKE_SYSTEM_PROCESSOR MATCHES "^aarch64") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^arm64") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^ARM64"))
            set(VULKAN_FFMPEG_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/winarm64/lib" CACHE PATH "Path to FFMPEG libs directory" FORCE)
            message(STATUS "FFMPEG Windows ARM64 lib location ${VULKAN_FFMPEG_LIB_PATH}")
            set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/winarm64/include" CACHE PATH "Path to FFMPEG include directory" FORCE)
            message(STATUS "FFMPEG Windows ARM64 include location ${FFMPEG_INCLUDE_DIR}")
        elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "^arm") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "^ARM"))
            set(VULKAN_FFMPEG_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/winarm/lib" CACHE PATH "Path to FFMPEG libs directory" FORCE)
            message(STATUS "FFMPEG Windows ARM64 lib location ${VULKAN_FFMPEG_LIB_PATH}")
            set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/winarm/include" CACHE PATH "Path to FFMPEG include directory" FORCE)
            message(STATUS "FFMPEG Windows ARM64 include location ${FFMPEG_INCLUDE_DIR}")
        else()
            set(VULKAN_FFMPEG_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/win64/lib" CACHE PATH "Path to FFMPEG libs directory" FORCE)
            message(STATUS "FFMPEG Windows x86_64 lib location ${VULKAN_FFMPEG_LIB_PATH}")
            set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/ffmpeg/win64/include" CACHE PATH "Path to FFMPEG include directory" FORCE)
            message(STATUS "FFMPEG Windows x86_64 include location ${FFMPEG_INCLUDE_DIR}")
        endif()
    else()
        message( STATUS "The location of the FFMPEG Lib: ${VULKAN_FFMPEG_LIB_PATH} expected the library to be installed to the regular system libs location" )
    endif()
endif()
############ VULKAN_FFMPEG_LIB_PATH ######################################

# find_package(FFmpeg REQUIRED)
include(FindFFmpeg)
set(FFMPEG_AVAILABLE OFF)

# FFMPEG_FOUND - system has ffmpeg or libav
# FFMPEG_INCLUDE_DIR - the ffmpeg include directory
# FFMPEG_LIBRARIES
# FFMPEG_LIBAVCODEC
# FFMPEG_LIBAVFORMAT
# FFMPEG_LIBAVUTIL
if(FFMPEG_FOUND)
    message("Found FFMPEG/LibAV libraries")
    include_directories(${FFMPEG_INCLUDE_DIR})
    set(FFMPEG_AVAILABLE ON)
else()
    if(WIN32)
       find_library(AVCODEC_LIB NAMES avcodec PATHS ${VULKAN_FFMPEG_LIB_PATH})
       message(STATUS ${AVCODEC_LIB})
       find_library(AVFORMAT_LIB NAMES avformat PATHS ${VULKAN_FFMPEG_LIB_PATH})
       message(STATUS ${AVFORMAT_LIB})
       find_library(AVUTIL_LIB NAMES avutil PATHS ${VULKAN_FFMPEG_LIB_PATH})
       message(STATUS ${AVUTIL_LIB})

       if(AVCODEC_LIB AND AVFORMAT_LIB AND AVUTIL_LIB)
           message(STATUS "Found FFMPEG libraries manually")
           set(FFMPEG_AVAILABLE ON)
           include_directories(${FFMPEG_INCLUDE_DIR})

           install(DIRECTORY "${VULKAN_FFMPEG_LIB_PATH}/../bin/"
           DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
                PATTERN "*.def" EXCLUDE
                PATTERN "*.a" EXCLUDE)

           install(DIRECTORY "${VULKAN_FFMPEG_LIB_PATH}/"
                DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")
       else()
            message(WARNING "Can't find libavcodec, libavformat, or libavutil on Windows!")
       endif()
    else()
        message(WARNING "Can't find libavcodec, libavformat, or libavutil on Linux!")
    endif()
endif()

if (NOT FFMPEG_AVAILABLE)
    message(WARNING "FFMPEG demuxing is not going to be supported.")
endif()

# Platform specific settings
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(LinuxSettings)
elseif(WIN32)
    include(WindowsSettings)
endif()

# Common compile definitions
add_definitions(
    -DVK_ENABLE_BETA_EXTENSIONS
    -DVK_USE_VIDEO_QUEUE
    -DVK_USE_VIDEO_DECODE_QUEUE
    -DVK_USE_VIDEO_ENCODE_QUEUE
    -DAPI_NAME="${API_NAME}"
)

# Encoder preprocess compute filter (VulkanFilterYuvCompute).
#
# ON by default because upstream has no gate at all: the filter path there is
# unconditional and EncoderConfig::enablePreprocessComputeFilter defaults to
# true. Leaving the guard undefined silently drops the filter from
# vk-video-enc-test too, which is not what the guard is for.
#
# The guard is for embedders that cannot supply a GLSL compiler, which the
# filter needs to compile its generated GLSL at runtime
# (VulkanShaderCompiler.cpp). No such embedder remains: standalone builds get a
# backend from cmake/VulkanShaderCompilerBackend.cmake (glslang by default,
# shaderc via -DVK_VIDEO_SAMPLES_SHADER_BACKEND=shaderc), and Chromium defines
# the macro too, in third_party/vulkan_video_samples/BUILD.gn
# config("private_includes").
#
# A COMPILE-TIME switch, and a real one. VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
# is consulted by guards in VkEncoderConfig.h, VkVideoEncoder.{h,cpp},
# vulkan_video_encoder_ext.cpp and the encoder-ext filter test. OFF drops the
# preprocess compute filter's code, and with it the EncoderConfig fields
# filterType and enablePreprocessComputeFilter -- which is what lets an embedder
# with no GLSL compiler backend build the encoder at all.
#
# Separately there is a RUNTIME flag of the same name,
# EncoderConfig::enablePreprocessComputeFilter (default true), which
# VkVideoEncoder::InitEncoder branches on. It exists only in an ON build;
# nothing in-tree clears it and no CLI or ext-API exposes it.
#
# Running without the filter is a supported configuration: the input image is
# then the encode source, so InitEncoder refuses any input format the driver does
# not advertise for the profile, with VK_ERROR_FORMAT_NOT_SUPPORTED. That refusal
# is unconditional in an OFF build and gated on the runtime flag in an ON one.
# Nothing that would need converting is accepted, which includes the default
# EncoderConfig::input.vkFormat.
#
# The four encoder-ext tests that require the filter define the macro on their
# own targets, so this option does not reach them.
#
# OFF IS NOT EXERCISED BY THE DEFAULT BUILD. It was broken -- one use of the
# runtime flag sat outside the guard declaring it -- until that was fixed and
# both settings were built. Configure it explicitly after touching filter code.
option(BUILD_ENCODER_COMPUTE_FILTER
       "Build the encoder's preprocess compute filter (requires a GLSL compiler backend)" ON)
if(BUILD_ENCODER_COMPUTE_FILTER)
    add_definitions(-DVK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED=1)
endif()

# Common include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

############ VULKAN_VIDEO_APIS_INCLUDE ######################################
if (DEFINED ENV{VULKAN_VIDEO_APIS_INCLUDE_PATH})
    MESSAGE(STATUS "VULKAN_VIDEO_APIS_INCLUDE_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_APIS_INCLUDE_PATH}")
    set(VULKAN_VIDEO_APIS_INCLUDE  "$ENV{VULKAN_VIDEO_APIS_INCLUDE_PATH}" CACHE PATH "Path to Vulkan Video include directory" FORCE)
else()
    set(VULKAN_VIDEO_APIS_INCLUDE  "${VULKAN_VIDEO_APIS_INCLUDE}" CACHE PATH "Path to Vulkan Video include directory")
endif()

if (EXISTS "${VULKAN_VIDEO_APIS_INCLUDE}")
    MESSAGE(STATUS "VULKAN_VIDEO_APIS_INCLUDE is set and valid ${VULKAN_VIDEO_APIS_INCLUDE}")
else()
    MESSAGE(STATUS "VULKAN_VIDEO_APIS_INCLUDE path is not set. Setting the default path location to ${PROJECT_SOURCE_DIR}/../vk_video_decoder/include")
    set(VULKAN_VIDEO_APIS_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/common/include" CACHE PATH "Path to Vulkan Video include directory" FORCE)
endif()
############ VULKAN_VIDEO_APIS_INCLUDE ######################################

# Library paths
set(VK_VIDEO_COMMON_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/common/libs")
set(VK_VIDEO_DECODER_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs")
set(VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/libs")
set(VK_VIDEO_DECODER_LIBS_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/include" CACHE STRING "Root path to video decoder libs includes")
set(VK_VIDEO_ENCODER_LIBS_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/include" CACHE STRING "Root path to video encoder libs includes")

# Enable cmake folders
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(LVL_TARGET_FOLDER lvl_cmake_targets)
set (PYTHON_CMD ${PYTHON_EXECUTABLE})

# Define macro used for building vkxml generated files
macro(run_vk_xml_generate dependency output)
    add_custom_command(OUTPUT ${output}
    COMMAND ${PYTHON_CMD} ${SCRIPTS_DIR}/lvl_genvk.py -registry ${SCRIPTS_DIR}/vk.xml ${output}
    DEPENDS ${SCRIPTS_DIR}/vk.xml ${SCRIPTS_DIR}/generator.py ${SCRIPTS_DIR}/${dependency} ${SCRIPTS_DIR}/lvl_genvk.py ${SCRIPTS_DIR}/reg.py
    )
endmacro()

# Define macro used for generating header files containing commit IDs for external dependencies
macro(run_external_revision_generate source_dir symbol_name output)
    add_custom_command(OUTPUT ${output}
    # NOTE: If you modify this call to use --rev_file instead of --git_dir (to read the commit ID from a file instead of
    # parsing from a Git repository), you probably also want to add the revision file to the list of DEPENDS on the
    # subsequent line (to ensure that the script is re-run when the revision file is modified).
    COMMAND ${PYTHON_CMD} ${SCRIPTS_DIR}/external_revision_generator.py --git_dir ${source_dir} -s ${symbol_name} -o ${output}
    DEPENDS ${SCRIPTS_DIR}/external_revision_generator.py ${source_dir}/.git/HEAD ${source_dir}/.git/index
    )
endmacro()

# Rules to build generated helper files
run_vk_xml_generate(loader_extension_generator.py vk_layer_dispatch_table.h)
run_vk_xml_generate(dispatch_table_helper_generator.py vk_dispatch_table_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_safe_struct.h)
run_vk_xml_generate(helper_file_generator.py vk_safe_struct.cpp)
run_vk_xml_generate(helper_file_generator.py vk_enum_string_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_object_types.h)
run_vk_xml_generate(helper_file_generator.py vk_extension_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_typemap_helper.h)

# Generate the dispatch table into the build tree. Shared with the external
# projects that embed VkCodecUtils sources, so they generate it the same way;
# exports VK_DISPATCH_TABLE_SOURCE and adds the include directory.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/VulkanDispatchTable.cmake)

# Layer Utils Library
# For Windows, we use a static lib because the Windows loader has a fairly restrictive loader search
# path that can't be easily modified to point it to the same directory that contains the layers.
set(VKVIDEO_UTILS_VLF_SOURCES
    common/layers/vk_format_utils.cpp
    )

if (WIN32)
    add_library(VkVideoUtils STATIC ${VKVIDEO_UTILS_VLF_SOURCES})
    target_compile_definitions(VkVideoUtils PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    add_library(VkVideoUtils SHARED ${VKVIDEO_UTILS_VLF_SOURCES})
    if(INSTALL_LVL_FILES)
        install(TARGETS VkVideoUtils DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endif()

target_include_directories(
    VkVideoUtils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan
    ${CMAKE_CURRENT_BINARY_DIR}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    )

# Build components based on options

############ VULKAN_VIDEO_PARSER_LIB ######################################
if(BUILD_VIDEO_PARSER)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs/NvVideoParser")
        set(LIBNVPARSER_BINARY_ROOT "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" CACHE STRING "User defined path to the the parser library binaries for this project")
    else()
       set(LIBNVPARSER_BINARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}" CACHE STRING "User defined path to the the parser library binaries for this project")
    endif()
    message(STATUS "LIBNVPARSER_BINARY_ROOT is at ${LIBNVPARSER_BINARY_ROOT}" )

    if (DEFINED ENV{VULKAN_VIDEO_PARSER_INCLUDE_PATH})
        MESSAGE(STATUS "VULKAN_VIDEO_PARSER_INCLUDE_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_PARSER_INCLUDE_PATH}")
        set(VULKAN_VIDEO_PARSER_INCLUDE  "$ENV{VULKAN_VIDEO_PARSER_INCLUDE_PATH}" CACHE PATH "Path to Vulkan Video Parser include directory" FORCE)
    else()
        set(VULKAN_VIDEO_PARSER_INCLUDE  "${VULKAN_VIDEO_PARSER_INCLUDE}" CACHE PATH "Path to Vulkan Video Parser include directory")
    endif()

    if (EXISTS "${VULKAN_VIDEO_PARSER_INCLUDE}")
        MESSAGE(STATUS "VULKAN_VIDEO_PARSER_INCLUDE is set and valid ${VULKAN_VIDEO_PARSER_INCLUDE}")
    else()
        MESSAGE(STATUS "VULKAN_VIDEO_PARSER_INCLUDE path is not set. Setting the default path location to ${CMAKE_CURRENT_SOURCE_DIR}/include/vkvideo_parser")
        set(VULKAN_VIDEO_PARSER_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/include/vkvideo_parser" CACHE PATH "Path to Vulkan Video Parser include directory" FORCE)
    endif()
    if (DEFINED ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH})
        MESSAGE(STATUS "VULKAN_VIDEO_PARSER_LIB_DIR_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH}")
        set(VULKAN_VIDEO_PARSER_LIB_PATH  "$ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH}" CACHE PATH "Path to Vulkan Video Parser library directory" FORCE)
    else()
        set(VULKAN_VIDEO_PARSER_LIB_PATH  "${LIBNVPARSER_BINARY_ROOT}" CACHE PATH "Path to Vulkan Video Parser library directory")
    endif()

    set(VULKAN_VIDEO_PARSER_LIB nvidia-vkvideo-parser CACHE PATH "The name of Vulkan Video Parser library")
    set(VULKAN_VIDEO_PARSER_STATIC_LIB nvidia-vkvideo-parser-static CACHE PATH "The name of Vulkan Video Parser library")
    set(VULKAN_VIDEO_PARSER_LIB_VERSION 1 CACHE PATH "The version of Vulkan Video Parser library")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs/NvVideoParser")

    else()
        if (EXISTS "${VULKAN_VIDEO_PARSER_LIB_PATH}")
            message(STATUS "Trying to find Vulkan Video Parser library at ${VULKAN_VIDEO_PARSER_LIB_PATH}" )
            find_library(VULKAN_VIDEO_PARSER_LIB_WITH_PATH NAMES ${VULKAN_VIDEO_PARSER_LIB} PATHS ${VULKAN_VIDEO_PARSER_LIB_PATH})
            message(STATUS "Vulkan Video Parser Lib: ${VULKAN_VIDEO_PARSER_LIB_WITH_PATH}" )
        endif()
endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs/NvVideoParser")
        add_subdirectory(vk_video_decoder/libs/NvVideoParser)
    else()
        message(WARNING "video_parser directory not found, skipping...")
        set(BUILD_VIDEO_PARSER OFF)
    endif()
endif()
############ VULKAN_VIDEO_PARSER_LIB ######################################

############ VULKAN_VIDEO_DECODER_LIB ######################################
    if(BUILD_DECODER)
    if (DEFINED ENV{VULKAN_DECODER_APIS_INCLUDE_PATH})
        MESSAGE(STATUS "VULKAN_DECODER_APIS_INCLUDE_PATH ENV VAR is set to $ENV{VULKAN_DECODER_APIS_INCLUDE_PATH}")
        set(VULKAN_DECODER_APIS_INCLUDE  "$ENV{VULKAN_DECODER_APIS_INCLUDE_PATH}" CACHE PATH "Path to Vulkan Video decode include directory" FORCE)
    else()
        set(VULKAN_DECODER_APIS_INCLUDE  "${VULKAN_DECODER_APIS_INCLUDE}" CACHE PATH "Path to Vulkan Video decode include directory")
    endif()
    if (EXISTS "${VULKAN_DECODER_APIS_INCLUDE}")
        MESSAGE(STATUS "VULKAN_DECODER_APIS_INCLUDE is set and valid ${VULKAN_DECODER_APIS_INCLUDE}")
    else()
        MESSAGE(STATUS "VULKAN_DECODER_APIS_INCLUDE path is not set. Setting the default path location to ${PROJECT_SOURCE_DIR}/include")
        set(VULKAN_DECODER_APIS_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/include" CACHE PATH "Path to Vulkan Video decode include directory" FORCE)
    endif()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs/VkVideoDecoder")
        set(LIBVKVIDEODECODER_BINARY_ROOT "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" CACHE STRING "User defined path to the the decoder library binaries for this project")
    else()
       set(LIBVKVIDEODECODER_BINARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bin/libs/vk_video_decoder/${LIB_ARCH_DIR}" CACHE STRING "User defined path to the the decoder library binaries for this project")
    endif()
    message(STATUS "LIBVKVIDEODECODER_BINARY_ROOT is at ${LIBVKVIDEODECODER_BINARY_ROOT}" )

    if (DEFINED ENV{VULKAN_VIDEO_DECODER_LIB_DIR_PATH})
        MESSAGE(STATUS "VULKAN_VIDEO_DECODER_LIB_DIR_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_DECODER_LIB_DIR_PATH}")
        set(VULKAN_VIDEO_DECODER_LIB_PATH  "$ENV{VULKAN_VIDEO_DECODER_LIB_DIR_PATH}" CACHE PATH "Path to Vulkan Video Decoder library directory" FORCE)
    else()
        set(VULKAN_VIDEO_DECODER_LIB_PATH  "${LIBVKVIDEODECODER_BINARY_ROOT}" CACHE PATH "Path to Vulkan Video Decoder library directory")
    endif()

    set(VULKAN_VIDEO_DECODER_LIB vkvideo-decoder CACHE PATH "The name of Vulkan Video Decoder library")
    set(VULKAN_VIDEO_DECODER_STATIC_LIB vkvideo-decoder-static CACHE PATH "The name of Vulkan Video Decoder static library")
    set(VULKAN_VIDEO_DECODER_LIB_VERSION 1 CACHE PATH "The version of Vulkan Video Decoder library")

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs")

    else()
        if (EXISTS "${VULKAN_VIDEO_DECODER_LIB_PATH}")
            message(STATUS "Trying to find Vulkan Video Decoder library at ${VULKAN_VIDEO_DECODER_LIB_PATH}" )
            find_library(VULKAN_VIDEO_DECODER_LIB_WITH_PATH NAMES ${VULKAN_VIDEO_DECODER_LIB} PATHS ${VULKAN_VIDEO_DECODER_LIB_PATH})
            message(STATUS "Vulkan Video Decoder Lib: ${VULKAN_VIDEO_DECODER_LIB_WITH_PATH}" )
        endif()
    endif()

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_decoder/libs")
        add_subdirectory(vk_video_decoder/libs)
    else()
        message(WARNING "vk_video_decoder libs directory not found, skipping...")
        set(BUILD_DECODER OFF)
    endif()

    if(BUILD_TESTS AND NOT DEFINED DEQP_TARGET)
        add_subdirectory(vk_video_decoder/test/vulkan-video-simple-dec)
        add_subdirectory(vk_video_decoder/test/vulkan-video-dec)
    endif()

    if(BUILD_DEMOS AND NOT DEFINED DEQP_TARGET)
        add_subdirectory(vk_video_decoder/demos)
    endif()
endif()
############ VULKAN_VIDEO_DECODER_LIB ######################################

############ NV_AQ_GPU_LIB Integration ######################################
# Check if NV_AQ_GPU_LIB is set and valid for building aq-vulkan library
# This must be done BEFORE building encoder library so definitions are available
if(DEFINED NV_AQ_GPU_LIB)
    # Ensure it's an absolute path
    get_filename_component(NV_AQ_GPU_LIB "${NV_AQ_GPU_LIB}" ABSOLUTE)

    if(EXISTS "${NV_AQ_GPU_LIB}" AND EXISTS "${NV_AQ_GPU_LIB}/CMakeLists.txt")
        message(STATUS "NV_AQ_GPU_LIB is set and valid: ${NV_AQ_GPU_LIB}")
        message(STATUS "Building aq-vulkan library from: ${NV_AQ_GPU_LIB}")

        # Pass the encoder include directory to the AQ library build
        # This allows AQ headers to find VkVideoGopStructure.h
        set(VK_VIDEO_ENCODER_INCLUDE_FOR_AQ "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/libs/VkVideoEncoder" CACHE PATH "Path to VkVideoEncoder headers for AQ library")

        # Add the aq-vulkan subdirectory
        add_subdirectory(${NV_AQ_GPU_LIB} ${CMAKE_BINARY_DIR}/aq_vulkan)

        # Set global C define to indicate AQ GPU library support is available
        add_definitions(-DNV_AQ_GPU_LIB_SUPPORTED=1)

        # Set CMake variable with library location for other targets to use
        set(NV_AQ_GPU_LIB_AVAILABLE TRUE CACHE BOOL "NV AQ GPU library is available")
        set(NV_AQ_GPU_LIB_TARGET "nvenc_aq_vulkan" CACHE STRING "Name of the NV AQ GPU library target")

        # Set up all AQ include directories in one place
        # NV_AQ_GPU_LIB points to aq-vulkan, so aq_common is one level up
        get_filename_component(AQ_COMMON_DIR "${NV_AQ_GPU_LIB}/../aq_common" ABSOLUTE)

        # Create a list of all required include directories
        set(NV_AQ_GPU_LIB_INCLUDE_DIRS "")

        # 1. aq_common base directory (for includes like "interface/EncodeAqAnalyzes.h")
        if(EXISTS "${AQ_COMMON_DIR}/interface/EncodeAqAnalyzes.h")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${AQ_COMMON_DIR}")
            message(STATUS "Found AQ common base directory: ${AQ_COMMON_DIR}")
        else()
            message(WARNING "EncodeAqAnalyzes.h not found at ${AQ_COMMON_DIR}/interface/EncodeAqAnalyzes.h")
        endif()

        # 2. aq_common/interface directory (for direct includes like "EncodeAqAnalyzes.h")
        if(EXISTS "${AQ_COMMON_DIR}/interface")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${AQ_COMMON_DIR}/interface")
            message(STATUS "Found AQ interface directory: ${AQ_COMMON_DIR}/interface")
        else()
            message(WARNING "aq_common/interface directory not found: ${AQ_COMMON_DIR}/interface")
        endif()

        # 3. aq_common/inc directory (for AqProcessor.h, aq_dump_manager.h, etc.)
        if(EXISTS "${AQ_COMMON_DIR}/inc")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${AQ_COMMON_DIR}/inc")
            message(STATUS "Found AQ common includes: ${AQ_COMMON_DIR}/inc")
        else()
            message(WARNING "aq_common/inc directory not found: ${AQ_COMMON_DIR}/inc")
        endif()

        # 4. aq-vulkan/inc directory (for VulkanGpuTemporalAQ.h and Vulkan-specific headers)
        if(EXISTS "${NV_AQ_GPU_LIB}/inc")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${NV_AQ_GPU_LIB}/inc")
            message(STATUS "Found AQ Vulkan includes: ${NV_AQ_GPU_LIB}/inc")
        else()
            message(WARNING "aq-vulkan/inc directory not found: ${NV_AQ_GPU_LIB}/inc")
        endif()

        # 5. VkVideoEncoder directory (for VkVideoGopStructure.h that AQ library needs)
        set(VK_VIDEO_ENCODER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/libs/VkVideoEncoder")
        if(EXISTS "${VK_VIDEO_ENCODER_DIR}")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${VK_VIDEO_ENCODER_DIR}")
            message(STATUS "Found VkVideoEncoder includes: ${VK_VIDEO_ENCODER_DIR}")
        else()
            message(WARNING "VkVideoEncoder directory not found: ${VK_VIDEO_ENCODER_DIR}")
        endif()

        # 6. Common libs directory (for dependencies of VkVideoGopStructure.h)
        set(COMMON_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common/libs")
        if(EXISTS "${COMMON_LIBS_DIR}")
            list(APPEND NV_AQ_GPU_LIB_INCLUDE_DIRS "${COMMON_LIBS_DIR}")
            message(STATUS "Found common libs includes: ${COMMON_LIBS_DIR}")
        endif()

        # Cache the include directories list
        set(NV_AQ_GPU_LIB_INCLUDE_DIRS "${NV_AQ_GPU_LIB_INCLUDE_DIRS}" CACHE STRING "List of all AQ include directories")
        message(STATUS "NV_AQ_GPU_LIB_INCLUDE_DIRS set to: ${NV_AQ_GPU_LIB_INCLUDE_DIRS}")

        message(STATUS "NV_AQ_GPU_LIB_SUPPORTED define will be set")
        message(STATUS "NV_AQ_GPU_LIB_TARGET: ${NV_AQ_GPU_LIB_TARGET}")
    else()
        message(WARNING "NV_AQ_GPU_LIB is set to '${NV_AQ_GPU_LIB}' but directory or CMakeLists.txt not found. Skipping aq-vulkan integration.")
        set(NV_AQ_GPU_LIB_AVAILABLE FALSE CACHE BOOL "NV AQ GPU library is available")
    endif()
else()
    message(STATUS "NV_AQ_GPU_LIB not set - aq-vulkan integration disabled")
    set(NV_AQ_GPU_LIB_AVAILABLE FALSE CACHE BOOL "NV AQ GPU library is available")
endif()
############ NV_AQ_GPU_LIB Integration ######################################

############ VULKAN_VIDEO_ENCODER_LIB ######################################
if(BUILD_ENCODER)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/libs/VkVideoEncoder")
        set(LIBVKVIDEOENCODER_BINARY_ROOT "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" CACHE STRING "User defined path to the the encoder library binaries for this project")
    else()
       set(LIBVKVIDEOENCODER_BINARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/bin/libs/vk_video_encoder/${LIB_ARCH_DIR}" CACHE STRING "User defined path to the the encoder library binaries for this project")
    endif()
    message(STATUS "LIBVKVIDEOENCODER_BINARY_ROOT is at ${LIBVKVIDEOENCODER_BINARY_ROOT}" )

    if (DEFINED ENV{VULKAN_VIDEO_ENCODER_INCLUDE_PATH})
        MESSAGE(STATUS "VULKAN_VIDEO_ENCODER_INCLUDE_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_ENCODER_INCLUDE_PATH}")
        set(VULKAN_VIDEO_ENCODER_INCLUDE  "$ENV{VULKAN_VIDEO_ENCODER_INCLUDE_PATH}" CACHE PATH "Path to Vulkan Video Encoder include directory" FORCE)
    else()
        set(VULKAN_VIDEO_ENCODER_INCLUDE  "${VULKAN_VIDEO_ENCODER_INCLUDE}" CACHE PATH "Path to Vulkan Video Encoder include directory")
    endif()

    if (EXISTS "${VULKAN_VIDEO_ENCODER_INCLUDE}")
        MESSAGE(STATUS "VULKAN_VIDEO_ENCODER_INCLUDE is set and valid ${VULKAN_VIDEO_ENCODER_INCLUDE}")
    else()
        MESSAGE(STATUS "VULKAN_VIDEO_ENCODER_INCLUDE path is not set. Setting the default path location to ${CMAKE_CURRENT_SOURCE_DIR}/include")
        set(VULKAN_VIDEO_ENCODER_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/include" CACHE PATH "Path to Vulkan Video Encoder include directory" FORCE)
    endif()
    if (DEFINED ENV{VULKAN_VIDEO_ENCODER_LIB_DIR_PATH})
        MESSAGE(STATUS "VULKAN_VIDEO_ENCODER_LIB_DIR_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_ENCODER_LIB_DIR_PATH}")
        set(VULKAN_VIDEO_ENCODER_LIB_PATH  "$ENV{VULKAN_VIDEO_ENCODER_LIB_DIR_PATH}" CACHE PATH "Path to Vulkan Video Encoder library directory" FORCE)
    else()
        set(VULKAN_VIDEO_ENCODER_LIB_PATH  "${LIBVKVIDEOENCODER_BINARY_ROOT}" CACHE PATH "Path to Vulkan Video Encoder library directory")
    endif()

    set(VULKAN_VIDEO_ENCODER_LIB vkvideo-encoder CACHE PATH "The name of Vulkan Video Encoder library")
    set(VULKAN_VIDEO_ENCODER_STATIC_LIB vkvideo-encoder-static CACHE PATH "The name of Vulkan Video Encoder library")
    set(VULKAN_VIDEO_ENCODER_LIB_VERSION 1 CACHE PATH "The version of Vulkan Video Encoder library")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/VkVideoEncoder")

    else()
        if (EXISTS "${VULKAN_VIDEO_ENCODER_LIB_PATH}")
            message(STATUS "Trying to find Vulkan Video Encoder library at ${VULKAN_VIDEO_ENCODER_LIB_PATH}" )
            find_library(VULKAN_VIDEO_ENCODER_LIB_WITH_PATH NAMES ${VULKAN_VIDEO_ENCODER_LIB} PATHS ${VULKAN_VIDEO_ENCODER_LIB_PATH})
            message(STATUS "Vulkan Video Encoder Lib: ${VULKAN_VIDEO_ENCODER_LIB_WITH_PATH}" )
        endif()
    endif()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/libs")
        add_subdirectory(vk_video_encoder/libs)
    else()
        message(WARNING "vk_video_encoder libs directory not found, skipping...")
        set(BUILD_ENCODER OFF)
    endif()

    if(BUILD_TESTS AND NOT DEFINED DEQP_TARGET)
        add_subdirectory(vk_video_encoder/test/vulkan-video-enc)
        # Device-free coverage of the ext API's chained-descriptor walk. Links
        # the static encoder archive for the internal header's test seams.
        add_subdirectory(vk_video_encoder/test/encoder-ext-sync)
        # Real-device coverage of the per-frame RELEASE fence export. Skips
        # (CTest SKIP, exit 77) on a host with no encode-capable GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-release-fence)
        # Real-device coverage of the per-frame ACQUIRE fence's fd OWNERSHIP:
        # that every refusal exit consumes the caller's sync_fd, and that the
        # success path still consumes it exactly once. Needs a GPU for the
        # same reason its sibling does -- a real sync_fd needs a real queue
        # signal to export from, and the success case needs a real import.
        # Skips (CTest SKIP, exit 77) without one.
        add_subdirectory(vk_video_encoder/test/encoder-ext-acquire-fd)
        # Device-free coverage of the input-format taxonomy (which rung of the
        # adaptation ladder a given format needs), of the preprocess-conversion
        # decision the binder derives from it, and of the transfer-function
        # declaration. All are pure functions of their arguments; what the
        # filter then DOES with the pixels needs a GPU and lives in
        # vk_filter_test.
        add_subdirectory(vk_video_encoder/test/encoder-ext-filter)
        # Real-device REGRESSION test for a defect this guards against:
        # DrainPendingFrames() permanently disabled async assembly, and the
        # synchronous fallback publishes no completion record, so every frame
        # submitted after that call was encoded and then never became
        # acquirable. DrainAndRestartThreads() closed it. This is a plain
        # gating test -- it exits 0 and carries NO WILL_FAIL; see the note in
        # the test's own CMakeLists.txt for why leaving WILL_FAIL on would
        # have reported the working fix as a failure. Skips (CTest SKIP,
        # exit 77) without a GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-drain-assembly)
        # Device-free coverage of the CONTRACT the test above rests on: that
        # VkVideoEncoder::AssembleBitstreamData -- the synchronous assembly
        # path -- publishes NO completion record, in either output mode. Seven
        # sites in the tree are built on that premise, including the guard in
        # ProcessOrderedFrames and the WriteDataToFile contract comment, and
        # this entry is the ONLY thing that executes the function: the ext
        # surface cannot reach it (asyncAssembly is pinned on, a subscriber is
        # always registered, and ProcessOrderedFrames refuses the sync
        # fallback whenever one exists) and the file-based CLI that can reach
        # it exposes no VkVideoEncoder to observe. Subclasses the encoder with
        # a null device context, so there is no GPU, driver or display
        # dependence and nothing to skip.
        add_subdirectory(vk_video_encoder/test/encoder-sync-assembly)
        # Real-device coverage of the DIRECT submit's FIXED 8-slot wait
        # array. The direct-encode path assembles waits into
        # VkSemaphoreSubmitInfoKHR[8] and its fill loop is bounded by that
        # capacity, so surplus waits were DISCARDED with no log, no assert and
        # a SUCCESS status -- and because the ext layer appends the imported
        # acquire-fence semaphore LAST, that producer fence is the first
        # casualty. Two entries: the DIRECT subject, and a STAGED control that
        # runs the identical case against the non-truncating vector assembly,
        # which is what makes a green subject arm distinguishable from a test
        # that cannot fail. Skips (exit 77) without an encode-capable GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-direct-wait-capacity)
        # Real-device coverage of the ADOPT-mode SESSION: the embedder
        # owns the VkInstance and names the VkPhysicalDevice, and the
        # LIBRARY creates its own VkDevice on them and encodes. That
        # combination -- externalInstance + externalPhysicalDevice with
        # externalDevice left NULL -- is the supported embedding shape,
        # and this suite is the only thing in the tree that exercises
        # externalInstance, externalPhysicalDevice or externalDevice at
        # all: the only caller was the out-of-tree Chromium embedder.
        # Four entries: the ADOPT subject, an OWN control that makes a
        # red subject attributable to the adopted handles rather than to
        # the host, the physical-device pin, and config.validate over a
        # borrowed instance. Skips (exit 77) without an encode-capable
        # GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-adopt-device)
        # Real-device coverage of INPUT RESIDENCY on an OS-HANDLE
        # registration. Every other handleType under
        # vk_video_encoder/test was ..._VK_IMAGE -- eight registration sites,
        # zero OPAQUE_FD -- so the entire OS-handle import path, and every
        # rule the ext layer applies ONLY to an OS handle, had no test of any
        # kind. The residency read in SubmitRegisteredFrame is exactly such a
        # rule, and a suite that only registers VK_IMAGE takes its other arm
        # every time.
        #
        # The registration is a SELF-IMPORT: the test exports OPAQUE_FD from
        # the LIBRARY's own VkDevice and hands the fd straight back to it,
        # which is what Chromium's shipping CPU staging tier does and which
        # nothing in this tree did.
        #
        # It asserts on a library-side counter
        # (VkVideoEncoderInputResidencyInfo, added for it), NOT on a
        # validation-error count, and that is forced rather than preferred:
        # the two barrier programs the residency decision selects between are
        # both spec-clean and both leave the image in the same layout, so a
        # layer-based assertion would be green either way -- a test that
        # cannot fail. Five entries: the OPAQUE_FD + explicit-LOCAL subject,
        # a VK_IMAGE control over the IDENTICAL image that makes a red
        # subject attributable to handleType alone, an explicit FOREIGN and
        # an AUTO pin that together stop the fix degenerating into "never
        # acquire", and a mirror of the Chromium tier-3 layout declaration
        # which PASSES -- the design registered it WILL_FAIL and the hardware
        # disagreed, so the registration was corrected rather than the
        # measurement explained away; the suite's own CMakeLists records the
        # reasoning. Skips (exit 77) without an encode-capable GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-input-residency)
        # Real-device INPUT FORMAT MATRIX. VkEncClassifyInputFormat names
        # nine formats; whether one REGISTERS, and onto which input path,
        # is a fact about a device, a session and a descriptor that the
        # taxonomy table cannot see. This walks all nine plus two excluded
        # controls against the LIBRARY-OWNED device and asserts both halves
        # per arm -- the registration status AND the slot's resolved
        # inputPath, read from VkEncProbeResource rather than inferred from
        # the config flag, which is equally true of a registration that
        # resolved to STAGED. Each arm also runs a NEGATIVE control with
        # its own declaration withheld, so a fix that merely deleted a
        # refusal fails here. Skips (exit 77) without an encode-capable GPU.
        add_subdirectory(vk_video_encoder/test/encoder-ext-format-matrix)
        add_subdirectory(vk_video_encoder/test/encoder-ext-input-format-query)

        # The ENCODE half of the same matrix. The routing test above proves a
        # descriptor registers and a slot resolves; it contains no encode call
        # at all. This one fills each format with a known primaries pattern,
        # submits real frames and writes the bitstream out for an INDEPENDENT
        # decoder to judge -- and abandons a 3-plane row that does not resolve
        # to FILTER before submitting, because staging one is a measured hang.
        add_subdirectory(vk_video_encoder/test/encoder-ext-format-encode)

        # Real-device coverage of the NVIDIA 615.06 dma-buf IMPORT-ORDINAL
        # GUARD, which shipped with a coverage floor of exactly zero: no
        # test, CMake entry or CI file referenced it, and
        # VK_VIDEO_ENCODER_EXTERNAL_HANDLE_TYPE_DMA_BUF reached
        # VkEncImportExternalImage from NO encoder-ext test at all -- every
        # registration in the tree was VK_IMAGE or OPAQUE_FD, on both of
        # which the guard returns at its second line. The suite was green
        # with the workaround deleted.
        #
        # THE GUARD IS RETIRED. kVkEncImportOrdinalGuardCount is 0 at the
        # shipped default, so it takes no sacrificial imports and retains
        # nothing. Do not read the paragraph below as a description of a
        # working mitigation; it never was one.
        #
        # WHAT WAS MEASURED, on an A4000 against NVIDIA 615.06. An import is
        # damaged iff its process-global dma-buf import ordinal falls on one
        # of two residues mod 32. The guard consumes ordinals ahead of the
        # caller, so it moves WHICH imports are damaged and never HOW MANY:
        # the damage rate is 12.5% at K = 0, 1, 2, 3 and 4 alike. It is a
        # phase shift wearing the costume of a fix.
        #
        # It is also not recoverable downstream. Writing through a damaged
        # import is swallowed, and an immediate re-import at ordinal+1
        # rescued 0 of 8 -- once a buffer is damaged it stays damaged. The
        # damage splits in two: on one population the bytes ARE in the
        # buffer and only our Vulkan mapping is dead (an EGL import and a
        # plain CPU mmap of the same fd both read the frame perfectly); on
        # the other the producer's write never landed and the pixels are
        # gone for every reader. A phase-shifted control moved both
        # populations exactly as predicted, 13/13.
        #
        # WHAT THIS SUITE IS FOR NOW. It exports a REAL dma-buf from a real
        # NVIDIA VkDevice and imports it, with counting thunks in the device
        # dispatch table, and asserts the retirement is REAL: at K = 0 the
        # guard must take zero imports, retain nothing, and report DISABLED
        # -- while the same harness, given an explicit non-zero count, still
        # holds the armed build to its original claim. That second arm is
        # what keeps the retirement honest instead of merely quiet, and it
        # is why the mechanism was left in place rather than deleted.
        #
        # THE KILL SWITCH NO LONGER DISCRIMINATES, and must not be sold as
        # a mutation proof: at K = 0 the subject is already inert, so
        # VK_VIDEO_ENCODER_NO_IMPORT_ORDINAL_GUARD=1 leaves it green (0/21).
        # The live mutation proof is the explicit --guard-count=2 arm, which
        # goes red 11/21 against the retired library.
        #
        # Skips (exit 77) with no GPU, no dma-buf export, or a non-NVIDIA
        # GPU, where the guard refuses to run by design.
        add_subdirectory(vk_video_encoder/test/encoder-ext-import-ordinal-guard)

        # The same guard's REPORTING CHANNEL, device-free. The suite above
        # asks whether the retirement is real on hardware; this one asks
        # whether anything OUTSIDE the library can find out. The guard
        # used to answer only on stderr, which the shipping Chromium
        # configuration discards wholesale (silenceStdio), so both of its
        # failure paths were silent in the one deployment it exists for.
        # This gates the carrier that replaced that narration:
        # VkVideoEncoderImportGuardInfo, chainable onto the registration
        # status echo and onto the completion snapshot. It covers the
        # CARRIER only. At the shipped default a real dma-buf import on an
        # NVIDIA device reports DISABLED; COMPLETE and INCOMPLETE are
        # reachable only on a re-armed build, and belong to the suite above
        # and to hardware.
        add_subdirectory(vk_video_encoder/test/encoder-ext-import-guard)

        # The dma-buf import CONTENT probe. The two suites above are about a
        # RETIRED workaround and what it reported; this one is about a
        # DETECTOR -- what the driver actually put in the buffer -- and the
        # distinction decides what a green here means.
        #
        # Device-free, and it covers the DECISION: the predicate over every
        # (Y,U,V) liveness quadrant, the scorer over synthetic NV12 buffers
        # laid out with the plane offsets, padded row pitch and skipped rows a
        # real HOST_VISIBLE LINEAR image has, the once-per-REGISTRATION latch,
        # the oldest-damaged-first report ordering, the drain on retirement,
        # and VkVideoEncoderImportContentInfo on both of its chain points.
        #
        # It does NOT cover whether the probe fires on a buffer NVIDIA 615.06
        # really poisoned; that needs the broken driver and a real GBM dma-buf
        # import, and is proven on hardware or not at all. What it buys is
        # that the part which would otherwise be exercised only on the one
        # host with the broken driver is regression-tested everywhere. The
        # predicate is asserted quadrant by quadrant rather than spot-checked
        # because the CHROMA-ONLY version of it has produced a wrong
        # conclusion on this defect twice, and a fully legal black frame
        # (Y=16, U=V=128) is asserted CLEAN because that is the false positive
        # that would reroute working buffers.
        add_subdirectory(vk_video_encoder/test/encoder-ext-import-content)

        # H.264 Baseline must not emit CABAC. profile_idc 66 covers Baseline
        # and Constrained Baseline alike and neither admits CABAC:
        # entropy_coding_mode_flag must be 0 there. Device-free.
        #
        # The entropy coder is chosen by nobody: there is no command-line
        # switch for it, it is taken from the device
        # preferredStdEntropyCodingModeFlag, which is CABAC on this vendor
        # hardware. So an explicit --profile baseline emitted profile_idc 66
        # with entropy_coding_mode_flag 1 -- a non-conformant bitstream, and
        # reachable from the WebRTC default profile.
        #
        # Three parts: the rule over the whole profile x entropy matrix, the
        # emitted SPS/PPS pair, and the composition with the profile
        # auto-upgrade in InitProfileLevel(). The third is the one worth
        # having. That upgrade does NOT cover this case -- it is guarded by
        # profileIdc == INVALID and so never runs on an explicit request --
        # and the suite fails if someone later widens it to fire on requests
        # and assumes it subsumes the clamp.
        add_subdirectory(vk_video_encoder/test/encoder-h264-baseline-entropy)

        # GOP sequencing, device-free. The GOP machine decides encode order,
        # B-frame counts and B-frame positions before any picture exists, and
        # nothing else in this tree asserts those tables -- the sibling suites
        # set gopLength and then encode, which cannot tell a correct sequence
        # from a plausible one. Carried as its own subdirectory because it
        # compiles VkVideoGopStructure.cpp directly and links nothing else.
        add_subdirectory(vk_video_encoder/test/gop-structure)

        # Encoder quality round trip: encode, decode with ffmpeg, and compare
        # per-frame PSNR against the source. This is the only round-trip
        # quality measurement in the tree -- the encoder's own --psnr option
        # compares input against RECONSTRUCTED, inside the encoder, and so
        # cannot see a stream that decodes wrongly.
        #
        # gpu-labelled, and it needs ffmpeg and ffprobe besides. It exits 77
        # when the encoder binary or either tool is missing, which is the skip
        # this label requires, so a host that cannot run it reports SKIPPED
        # rather than red.
        #
        # --encoder is passed explicitly rather than letting the script derive
        # it from --samples-root: that derivation spells the build directory
        # in lower case and this tree builds into BUILD.
        add_test(NAME EncoderAv1QualityRoundTrip
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/test/av1_encoder_quality_test.py
                         --encoder $<TARGET_FILE:vulkan-video-enc-test>
                         --output-dir ${CMAKE_CURRENT_BINARY_DIR}/av1-quality
                         --num-frames 30
                         --codecs av1)
        set_tests_properties(EncoderAv1QualityRoundTrip PROPERTIES
                             SKIP_RETURN_CODE 77 LABELS "gpu")

        # The gate's own classification, device-free. The round trip above can
        # only exercise these decisions on a host that has an encoder, ffmpeg
        # and content -- and both of them are about reporting a run as
        # something it was not, so the hosts that cannot run it are exactly
        # the ones where a misclassification would go unnoticed.
        #
        # device-free, and with no SKIP_RETURN_CODE: there is nothing here to
        # skip for, and a skip is the failure mode under test.
        #
        # THE LABEL IS NOT OPTIONAL, and "not gpu" is not a label. Every
        # registered test must carry 'device-free' or 'gpu': those two name the
        # CI sets, and run_ctest_ci.sh audits the registry against them, so a
        # test with neither is in no set, gates nothing, and fails the audit
        # rather than quietly running nowhere.
        add_test(NAME EncoderAv1QualityGateClassification
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/vk_video_encoder/test/test_av1_quality_gate.py)
        set_tests_properties(EncoderAv1QualityGateClassification PROPERTIES
                             LABELS "device-free")
    endif()

    if(BUILD_DEMOS AND NOT DEFINED DEQP_TARGET)
        add_subdirectory(vk_video_encoder/demos)
    endif()
endif()
############ VULKAN_VIDEO_ENCODER_LIB ######################################

############ VK_FILTER_TEST - Filter Unit Tests ######################################
if(BUILD_TESTS AND NOT DEFINED DEQP_TARGET)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/common/libs/tests")
        message(STATUS "Building VulkanFilterYuvCompute test application")
        add_subdirectory(common/libs/tests)
    else()
        message(STATUS "Filter tests directory not found, skipping...")
    endif()
endif()
############ VK_FILTER_TEST ######################################