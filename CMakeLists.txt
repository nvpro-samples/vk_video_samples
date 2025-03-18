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
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# set (CMAKE_VERBOSE_MAKEFILE 1)

# Set module path for Find*.cmake files
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

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

# Add before the build components section
# Common dispatch table generation macro
macro(generate_dispatch_table out)
    add_custom_command(OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${SCRIPTS_DIR}/generate-dispatch-table.py ${out}
        DEPENDS ${SCRIPTS_DIR}/generate-dispatch-table.py
    )
endmacro()

# Generate the dispatch table files
generate_dispatch_table(${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.h)
generate_dispatch_table(${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.cpp)

add_custom_target(GenerateDispatchTables
    DEPENDS
        ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.h
        ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.cpp
)

# Layer Utils Library
# For Windows, we use a static lib because the Windows loader has a fairly restrictive loader search
# path that can't be easily modified to point it to the same directory that contains the layers.
set(VKVIDEO_UTILS_VLF_SOURCES
    ../common/layers/vk_format_utils.cpp
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
    endif()

    if(BUILD_DEMOS AND NOT DEFINED DEQP_TARGET)
        add_subdirectory(vk_video_encoder/demos)
    endif()
endif()
############ VULKAN_VIDEO_ENCODER_LIB ######################################