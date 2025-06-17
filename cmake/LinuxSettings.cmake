# Linux specific settings
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LIB_ARCH_DIR "linux_amd64_debug" CACHE STRING "User defined library target")
else()
    set(LIB_ARCH_DIR "linux_amd64_release" CACHE STRING "User defined library target")
endif()

# Ubuntu detection
file(READ "/etc/os-release" OS_RELEASE_CONTENT)
string(FIND "${OS_RELEASE_CONTENT}" "Ubuntu" UBUNTU_FOUND)
if(UBUNTU_FOUND GREATER -1)
    message(STATUS "Detected Ubuntu system, setting library path")
    set(CMAKE_INSTALL_LIBDIR "lib/x86_64-linux-gnu" CACHE PATH "Path to Ubuntu library dir")
else()
    message(STATUS "Non-Ubuntu Linux detected")
endif()

# WSI support options
option(BUILD_WSI_XCB_SUPPORT "Build XCB WSI support" ON)
option(BUILD_WSI_XLIB_SUPPORT "Build Xlib WSI support" ON)
option(BUILD_WSI_WAYLAND_SUPPORT "Build Wayland WSI support" ON)
option(BUILD_WSI_MIR_SUPPORT "Build Mir WSI support" OFF)
set(DEMOS_WSI_SELECTION "XCB" CACHE STRING "Select WSI target for demos (XCB, XLIB, WAYLAND, MIR, DISPLAY)")

# Find required packages
include(FindPkgConfig)
include(GNUInstallDirs)

# WSI Support Configuration
if(NOT DEMOS_WSI_SELECTION)
    set(DEMOS_WSI_SELECTION "XCB")
endif()

if(BUILD_WSI_XCB_SUPPORT)
    find_package(XCB REQUIRED)
    add_definitions(-DVK_USE_PLATFORM_XCB_KHR)
    set(DEMO_INCLUDE_DIRS
        ${XCB_INCLUDE_DIRS}
        ${DEMO_INCLUDE_DIRS}
    )
    link_libraries(${XCB_LIBRARIES})
endif()

if(BUILD_WSI_XLIB_SUPPORT)
    find_package(X11 REQUIRED)
    add_definitions(-DVK_USE_PLATFORM_XLIB_KHR)
    set(DEMO_INCLUDE_DIRS
        ${X11_INCLUDE_DIR}
        ${DEMO_INCLUDE_DIRS}
    )
    link_libraries(${X11_LIBRARIES})
endif()

if(BUILD_WSI_WAYLAND_SUPPORT)
    find_package(Wayland REQUIRED)
    include_directories(${WAYLAND_CLIENT_INCLUDE_DIR})
    add_definitions(-DVK_USE_PLATFORM_WAYLAND_KHR)
    set(DEMO_INCLUDE_DIRS
        ${WAYLAND_CLIENT_INCLUDE_DIR}
        ${DEMO_INCLUDE_DIRS}
    )
    link_libraries(${WAYLAND_CLIENT_LIBRARIES})
endif()

if(BUILD_WSI_MIR_SUPPORT)
    find_package(Mir REQUIRED)
endif()

# Set fallback paths
set(FALLBACK_CONFIG_DIRS "/etc/xdg" CACHE STRING
    "Search path to use when XDG_CONFIG_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")
set(FALLBACK_DATA_DIRS "/usr/local/share:/usr/share" CACHE STRING
    "Search path to use when XDG_DATA_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")

# Add definitions for fallback paths
add_definitions(-DFALLBACK_CONFIG_DIRS="${FALLBACK_CONFIG_DIRS}")
add_definitions(-DFALLBACK_DATA_DIRS="${FALLBACK_DATA_DIRS}")
add_definitions(-DSYSCONFDIR="${CMAKE_INSTALL_FULL_SYSCONFDIR}")

# Make sure /etc is searched by the loader
if(NOT (CMAKE_INSTALL_FULL_SYSCONFDIR STREQUAL "/etc"))
    add_definitions(-DEXTRASYSCONFDIR="/etc")
endif()

# Compiler flags for GCC/Clang
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(COMMON_COMPILE_FLAGS "-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wcast-qual")
    set(COMMON_COMPILE_FLAGS "${COMMON_COMPILE_FLAGS} -fno-strict-aliasing -fno-builtin-memcmp")

    # Warning about implicit fallthrough in switch blocks
    check_cxx_compiler_flag(-Wimplicit-fallthrough COMPILER_SUPPORTS_FALLTHROUGH_WARNING)
    if(COMPILER_SUPPORTS_FALLTHROUGH_WARNING)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wimplicit-fallthrough")
    endif()

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 ${COMMON_COMPILE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILE_FLAGS} -std=c++11 -fno-rtti")

    # Visibility flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
endif()

# Installation settings
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")

# Link with system libraries
link_libraries(${API_LOWERCASE} m)

# Include common system paths
include_directories("${PROJECT_SOURCE_DIR}/icd/common")

# Installation targets for LVL files
if(INSTALL_LVL_FILES)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/vulkan"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vk_layer_dispatch_table.h"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vulkan")
endif()

# Uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)