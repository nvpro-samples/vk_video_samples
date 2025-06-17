# The name of our project is "VULKAN_VIDEO_TESTS". CMakeLists files in this project can
# refer to the root source directory of the project as ${VULKAN_VIDEO_TESTS_SOURCE_DIR} and
# to the root binary directory of the project as ${VULKAN_BINARY_DIR}.
cmake_minimum_required(VERSION 3.20.0)

include(CheckCXXCompilerFlag)
include(FetchContent)

# This must come before the project command.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "Minimum OS X deployment version")

project (VULKAN_VIDEO_TESTS)
# set (CMAKE_VERBOSE_MAKEFILE 1)

# The API_NAME allows renaming builds to avoid conflicts with installed SDKs
# The MAJOR number of the version we're building, used in naming
# <api-name>-<major>.dll (and other files).
set(API_NAME "Vulkan" CACHE STRING "API name to use when building")
set(MAJOR "1")
string(TOLOWER ${API_NAME} API_LOWERCASE)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
find_package(PythonInterp 3 REQUIRED)

option(USE_CCACHE "Use ccache" OFF)
if (USE_CCACHE)
    find_program(CCACHE_FOUND ccache)
    if(CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
    endif(CCACHE_FOUND)
endif()

# Handle Vulkan and shaderc dependencies
include(FindVulkanAndDependencies)

############ LINUX LIBRARY includes ######################################
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        message(STATUS "Selected Linux build type: Debug")
        if(NOT DEFINED ENV{LIB_ARCH_DIR})
            set(LIB_ARCH_DIR "linux_amd64_debug" CACHE STRING "User defined library target")
        endif()
    else()
        message(STATUS "Selected Linux build type: Release")
        if(NOT DEFINED ENV{LIB_ARCH_DIR})
            set(LIB_ARCH_DIR "linux_amd64_release" CACHE STRING "User defined library target")
        endif()
    endif()
    message(STATUS "Selected Linux build binary arch: " ${LIB_ARCH_DIR})
    # Ubuntu detection
    file(READ "/etc/os-release" OS_RELEASE_CONTENT)
    string(FIND "${OS_RELEASE_CONTENT}" "Ubuntu" UBUNTU_FOUND)
    if(UBUNTU_FOUND GREATER -1)
        message(STATUS "Detected Ubuntu system, setting library path")
        set(CMAKE_INSTALL_LIBDIR "lib/x86_64-linux-gnu" CACHE PATH "Path to Ubuntu library dir")
    else()
        message(STATUS "Non-Ubuntu Linux detected")
    endif()
endif()
############ LINUX LIBRARY includes ######################################

############ WINDOWS LIBRARY includes ######################################
if(WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        message(STATUS "Selected Windows build type: Debug")
        if(NOT DEFINED ENV{LIB_ARCH_DIR})
            set(LIB_ARCH_DIR "wddm2_amd64_debug" CACHE STRING "User defined library target")
        endif()
    else()
        message(STATUS "Selected Windows build type: Release")
        if(NOT DEFINED ENV{LIB_ARCH_DIR})
            set(LIB_ARCH_DIR "wddm2_amd64_release" CACHE STRING "User defined library target")
        endif()
    endif()
    message(STATUS "Selected Windows build binary arch: " ${LIB_ARCH_DIR})
endif()
############ WINDOWS LIBRARY includes ######################################

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
    set(VULKAN_VIDEO_APIS_INCLUDE  "${PROJECT_SOURCE_DIR}/../vk_video_decoder/include" CACHE PATH "Path to Vulkan Video include directory" FORCE)
endif()
############ VULKAN_VIDEO_APIS_INCLUDE ######################################

############ VULKAN_VIDEO_ENCODER_INCLUDE ######################################
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/VkVideoEncoder")
    set(LIBVKVIDEOENCODER_BINARY_ROOT "${CMAKE_BINARY_DIR}/libs" CACHE STRING "User defined path to the the encoder library binaries for this project")
else()
   set(LIBVKVIDEOENCODER_BINARY_ROOT "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/vk_video_encoder/${LIB_ARCH_DIR}" CACHE STRING "User defined path to the the encoder library binaries for this project")
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
    set(VULKAN_VIDEO_ENCODER_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "Path to Vulkan Video Encoder include directory" FORCE)
endif()
############ VULKAN_VIDEO_ENCODER_INCLUDE ######################################

############ VULKAN_VIDEO_ENCODER_LIB_PATH ######################################
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
############ VULKAN_VIDEO_ENCODER_LIB_PATH ######################################

############ VULKAN_VIDEO_PARSER_INCLUDE ######################################
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/libs/NvVideoParser")
    set(LIBNVPARSER_BINARY_ROOT "${CMAKE_BINARY_DIR}/libs/NvVideoParser" CACHE STRING "User defined path to the the parser library binaries for this project")
else()
   set(LIBNVPARSER_BINARY_ROOT "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/bin/libs/nv_vkvideo_parser/${LIB_ARCH_DIR}" CACHE STRING "User defined path to the the parser library binaries for this project")
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
    MESSAGE(STATUS "VULKAN_VIDEO_PARSER_INCLUDE path is not set. Setting the default path location to ${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/include/vkvideo_parser")
    set(VULKAN_VIDEO_PARSER_INCLUDE  "${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/include/vkvideo_parser" CACHE PATH "Path to Vulkan Video Parser include directory" FORCE)
endif()
############ VULKAN_VIDEO_PARSER_INCLUDE ######################################

############ VULKAN_VIDEO_PARSER_LIB_PATH ######################################
if (DEFINED ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH})
    MESSAGE(STATUS "VULKAN_VIDEO_PARSER_LIB_DIR_PATH ENV VAR is set to $ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH}")
    set(VULKAN_VIDEO_PARSER_LIB_PATH  "$ENV{VULKAN_VIDEO_PARSER_LIB_DIR_PATH}" CACHE PATH "Path to Vulkan Video Parser library directory" FORCE)
else()
    set(VULKAN_VIDEO_PARSER_LIB_PATH  "${LIBNVPARSER_BINARY_ROOT}" CACHE PATH "Path to Vulkan Video Parser library directory")
endif()

set(VULKAN_VIDEO_PARSER_LIB nvidia-vkvideo-parser CACHE PATH "The name of Vulkan Video Parser library")
set(VULKAN_VIDEO_PARSER_STATIC_LIB nvidia-vkvideo-parser-static CACHE PATH "The name of Vulkan Video Parser library")
set(VULKAN_VIDEO_PARSER_LIB_VERSION 1 CACHE PATH "The version of Vulkan Video Parser library")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/libs/NvVideoParser")

else()
    if (EXISTS "${VULKAN_VIDEO_PARSER_LIB_PATH}")
        message(STATUS "Trying to find Vulkan Video Parser library at ${VULKAN_VIDEO_PARSER_LIB_PATH}" )
        find_library(VULKAN_VIDEO_PARSER_LIB_WITH_PATH NAMES ${VULKAN_VIDEO_PARSER_LIB} PATHS ${VULKAN_VIDEO_PARSER_LIB_PATH})
        message(STATUS "Vulkan Video Parser Lib: ${VULKAN_VIDEO_PARSER_LIB_WITH_PATH}" )
    endif()
endif()
############ VULKAN_VIDEO_PARSER_LIB_PATH ######################################

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
        set(VULKAN_FFMPEG_LIB_PATH "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/../vk_video_decoder/bin/libs/ffmpeg/win64/lib" CACHE PATH "Path to FFMPEG libs directory" FORCE)
        message(STATUS "FFMPEG lib location ${VULKAN_FFMPEG_LIB_PATH}")
        set(FFMPEG_INCLUDE_DIR "${VULKAN_VIDEO_TESTS_SOURCE_DIR}/../vk_video_decoder/bin/libs/ffmpeg/win64/include" CACHE PATH "Path to FFMPEG include directory" FORCE)
        message(STATUS "FFMPEG include location ${FFMPEG_INCLUDE_DIR}")
    else()
        message( STATUS "The location of the FFMPEG Lib: ${VULKAN_FFMPEG_LIB_PATH} expected the library to be installed to the regular system libs location" )
    endif()
endif()
############ VULKAN_FFMPEG_LIB_PATH ######################################

# Enable cmake folders
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(LVL_TARGET_FOLDER lvl_cmake_targets)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(FALLBACK_CONFIG_DIRS "/etc/xdg" CACHE STRING
        "Search path to use when XDG_CONFIG_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")
    set(FALLBACK_DATA_DIRS "/usr/local/share:/usr/share" CACHE STRING
        "Search path to use when XDG_DATA_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")
endif()

set(LIB_ARCH_DIR "$ENV{LIB_ARCH_DIR}" CACHE STRING "Set sufix for the built library target. For ex. Linux_amd64_debug")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(FindPkgConfig)
    option(BUILD_WSI_XCB_SUPPORT "Build XCB WSI support" ON)
    option(BUILD_WSI_XLIB_SUPPORT "Build Xlib WSI support" ON)
    option(BUILD_WSI_WAYLAND_SUPPORT "Build Wayland WSI support" ON)
    option(BUILD_WSI_MIR_SUPPORT "Build Mir WSI support" OFF)
    set(DEMOS_WSI_SELECTION "XCB" CACHE STRING "Select WSI target for demos (XCB, XLIB, WAYLAND, MIR, DISPLAY)")

    if (BUILD_WSI_XCB_SUPPORT)
        find_package(XCB REQUIRED)
    endif()

    if (BUILD_WSI_XLIB_SUPPORT)
        find_package(X11 REQUIRED)
    endif()

    if (BUILD_WSI_WAYLAND_SUPPORT)
        find_package(Wayland REQUIRED)
        include_directories(${WAYLAND_CLIENT_INCLUDE_DIR})
    endif()

    if (BUILD_WSI_MIR_SUPPORT)
        find_package(Mir REQUIRED)
    endif()

    SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_definitions(-DVK_USE_PLATFORM_WIN32_KHR -DWIN32_LEAN_AND_MEAN -DNOMINMAX="1")
    set(DisplayServer Win32)
    if (NOT MSVC_VERSION LESS 1900)
        # Enable control flow guard
        message(STATUS "Building demos with control flow guard")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/guard:cf>")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /guard:cf")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /guard:cf")
    endif()
    set(DEMO_INCLUDE_DIRS
       "${PROJECT_SOURCE_DIR}/icd/common"
        ${DEMO_INCLUDE_DIRS}
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (NOT DEMOS_WSI_SELECTION)
        set(DEMOS_WSI_SELECTION "XCB")
    endif()

    if (DEMOS_WSI_SELECTION STREQUAL "XCB")
        if (NOT BUILD_WSI_XCB_SUPPORT)
            message( FATAL_ERROR "Selected XCB for demos build but not building Xcb support" )
        endif()
        set(DEMO_INCLUDE_DIRS
            ${XCB_INCLUDE_DIRS}
            ${DEMO_INCLUDE_DIRS}
        )
        link_libraries(${XCB_LIBRARIES})
        add_definitions(-DVK_USE_PLATFORM_XCB_KHR)
    elseif(DEMOS_WSI_SELECTION STREQUAL "XLIB")
        if (NOT BUILD_WSI_XLIB_SUPPORT)
            message( FATAL_ERROR "Selected XLIB for demos build but not building Xlib support" )
        endif()
        set(DEMO_INCLUDE_DIRS
            ${X11_INCLUDE_DIR}
            ${DEMO_INCLUDE_DIRS}
        )
        link_libraries(${X11_LIBRARIES})
        add_definitions(-DVK_USE_PLATFORM_XLIB_KHR)
    elseif(DEMOS_WSI_SELECTION STREQUAL "WAYLAND")
        if (NOT BUILD_WSI_WAYLAND_SUPPORT)
            message( FATAL_ERROR "Selected Wayland for demos build but not building Wayland support" )
        endif()
        set(DEMO_INCLUDE_DIRS
            ${WAYLAND_CLIENT_INCLUDE_DIR}
            ${DEMO_INCLUDE_DIRS}
        )
        link_libraries(${WAYLAND_CLIENT_LIBRARIES})
        add_definitions(-DVK_USE_PLATFORM_WAYLAND_KHR)
    elseif(DEMOS_WSI_SELECTION STREQUAL "DISPLAY")
        add_definitions(-DVK_USE_PLATFORM_DISPLAY_KHR)
    else()
        message( FATAL_ERROR "Unrecognized value for DEMOS_WSI_SELECTION: ${DEMOS_WSI_SELECTION}" )
     endif()

    include_directories ("${PROJECT_SOURCE_DIR}/icd/common")
    link_libraries(${API_LOWERCASE} m)
else()
    message(FATAL_ERROR "Unsupported Platform!")
endif()

add_definitions(-DVK_USE_VIDEO_QUEUE)
add_definitions(-DVK_USE_VIDEO_DECODE_QUEUE)
add_definitions(-DVK_USE_VIDEO_ENCODE_QUEUE)
add_definitions(-DVK_ENABLE_BETA_EXTENSIONS)

find_package(FFmpeg REQUIRED)
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
    message(WARNING "FFMPEG demuxing is not going to be suppored.")
endif()

set(SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../scripts")

# Header file for CMake settings
include_directories("${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/../common/include" "${VULKAN_HEADERS_INCLUDE_DIR}" "${Vulkan_INCLUDE_DIR}")

if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(COMMON_COMPILE_FLAGS "-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wcast-qual")
    set(COMMON_COMPILE_FLAGS "${COMMON_COMPILE_FLAGS} -fno-strict-aliasing -fno-builtin-memcmp")

    # These flags are not supported on Windows and some older version of GCC
    # that our bots use.
    # Warning about implicit fallthrough in switch blocks
    check_cxx_compiler_flag(-Wimplicit-fallthrough COMPILER_SUPPORTS_FALLTHROUGH_WARNING)
    if (COMPILER_SUPPORTS_FALLTHROUGH_WARNING)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wimplicit-fallthrough")
    endif()

    if (APPLE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_COMPILE_FLAGS}")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 ${COMMON_COMPILE_FLAGS}")
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILE_FLAGS} -std=c++11 -fno-rtti")
    if (UNIX)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
    endif()
endif()

if(WIN32)
    # Treat warnings as errors
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/WX>")
    # Disable RTTI
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/GR->")
    # Warn about nested declarations
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34456>")
    # Warn about potentially uninitialized variables
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34701>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34703>")
    # Warn about different indirection types.
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34057>")
    # Warn about signed/unsigned mismatch.
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34245>")
    # Warn about deprecated functions
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4996>")
    # Warn about const-casting (cast-qual)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34100>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/w34101>")
endif()

if(NOT WIN32)
    set (BUILDTGT_DIR build)
    set (BINDATA_DIR Bin)
    set (LIBSOURCE_DIR Lib)
else()
    # is WIN32
    option(DISABLE_BUILD_PATH_DECORATION "Disable the decoration of the gslang and SPIRV-Tools build path with MSVC build type info" OFF)
    option(DISABLE_BUILDTGT_DIR_DECORATION "Disable the decoration of the gslang and SPIRV-Tools build path with target info" OFF)
    option(ENABLE_WIN10_ONECORE "Link the loader with OneCore umbrella libraries" OFF)

    # For Windows, since 32-bit and 64-bit items can co-exist, we build each in its own build directory.
    # 32-bit target data goes in build32, and 64-bit target data goes into build.  So, include/link the
    # appropriate data at build time.
    if (DISABLE_BUILDTGT_DIR_DECORATION)
        set (BUILDTGT_DIR "")
        set (BINDATA_DIR "")
        set (LIBSOURCE_DIR "")
    elseif (CMAKE_CL_64)
        set (BUILDTGT_DIR build)
        set (BINDATA_DIR Bin)
        set (LIBSOURCE_DIR Lib)
    else()
        set (BUILDTGT_DIR build32)
        set (BINDATA_DIR Bin32)
        set (LIBSOURCE_DIR Lib32)
    endif()
endif()

option(BUILD_LOADER "Build loader" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_DEMOS "Build demos" ON)
if (APPLE)
    option(BUILD_VKJSON "Build vkjson" OFF)
else()
    option(BUILD_VKJSON "Build vkjson" ON)
endif()
option(BUILD_ICD "Build icd" ON)

set(VK_VIDEO_COMMON_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../common/libs" CACHE STRING "Root path to common vkvideo libs sources")
set(VK_VIDEO_DECODER_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/libs" CACHE STRING "Root path to common vkvideo decoder libs sources")
set(VK_VIDEO_DECODER_LIBS_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}../vk_video_decoder/include" CACHE STRING "Root path to video decoder libs includes")
set(VK_VIDEO_ENCODER_LIBS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs" CACHE STRING "Root path to common vkvideo encoder libs sources")

if (WIN32)
    if(DISABLE_BUILD_PATH_DECORATION)
        set (DEBUG_DECORATION "")
        set (RELEASE_DECORATION "")
    else()
        set (DEBUG_DECORATION "Debug")
        set (RELEASE_DECORATION "Release")
    endif()
else()
    # not WIN32
endif()

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

# Custom target for generated vulkan helper file dependencies
add_custom_target(generate_helper_files DEPENDS
    vk_enum_string_helper.h
    vk_safe_struct.h
    vk_safe_struct.cpp
    vk_object_types.h
    vk_layer_dispatch_table.h
    vk_dispatch_table_helper.h
    vk_extension_helper.h
    vk_typemap_helper.h
    )
set_target_properties(generate_helper_files PROPERTIES FOLDER ${LVL_TARGET_FOLDER})

# Rules to build generated helper files
run_vk_xml_generate(loader_extension_generator.py vk_layer_dispatch_table.h)
run_vk_xml_generate(dispatch_table_helper_generator.py vk_dispatch_table_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_safe_struct.h)
run_vk_xml_generate(helper_file_generator.py vk_safe_struct.cpp)
run_vk_xml_generate(helper_file_generator.py vk_enum_string_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_object_types.h)
run_vk_xml_generate(helper_file_generator.py vk_extension_helper.h)
run_vk_xml_generate(helper_file_generator.py vk_typemap_helper.h)

if(NOT WIN32)
    include(GNUInstallDirs)

    add_definitions(-DFALLBACK_CONFIG_DIRS="${FALLBACK_CONFIG_DIRS}")
    add_definitions(-DFALLBACK_DATA_DIRS="${FALLBACK_DATA_DIRS}")
    add_definitions(-DSYSCONFDIR="${CMAKE_INSTALL_FULL_SYSCONFDIR}")

    # Make sure /etc is searched by the loader
    if(NOT (CMAKE_INSTALL_FULL_SYSCONFDIR STREQUAL "/etc"))
        add_definitions(-DEXTRASYSCONFDIR="/etc")
    endif()
endif()

if(UNIX)
    if(INSTALL_LVL_FILES)
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/vulkan" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vk_layer_dispatch_table.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vulkan")
    endif()

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()

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
add_dependencies(VkVideoUtils generate_helper_files)
target_include_directories(
    VkVideoUtils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan
    ${CMAKE_CURRENT_BINARY_DIR}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    )

add_definitions(-DAPI_NAME="${API_NAME}")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/libs/NvVideoParser")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../vk_video_decoder/libs/NvVideoParser ${CMAKE_CURRENT_BINARY_DIR}/libs/NvVideoParser)
else()
   install(DIRECTORY "${LIBNVPARSER_BINARY_ROOT}/"
           DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
           PATTERN "*.def" EXCLUDE
           PATTERN "*.a" EXCLUDE)
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs")
    add_subdirectory(libs)
else()
    install(DIRECTORY "${LIBVKVIDEOENCODER_BINARY_ROOT}/"
            DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            PATTERN "*.def" EXCLUDE
            PATTERN "*.a" EXCLUDE)
endif()

add_subdirectory(test/vulkan-video-enc)

if(BUILD_DEMOS AND NOT DEFINED DEQP_TARGET)
    add_subdirectory(demos)
endif()
