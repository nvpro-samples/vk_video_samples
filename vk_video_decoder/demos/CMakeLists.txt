set(CMAKE_WARN_DEPRECATED OFF)

set(DEMO_INCLUDE_DIRS
    ${CMAKE_CURRENT_BINARY_DIR}
    # Path to project wide vulkan helper generation
    ${CMAKE_CURRENT_BINARY_DIR}/../..
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_definitions(-DVK_USE_PLATFORM_WIN32_KHR -DWIN32_LEAN_AND_MEAN)
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

file(GLOB TEXTURES
  "${PROJECT_SOURCE_DIR}/demos/*.ppm"
  )
file(COPY ${TEXTURES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

if(WIN32)
  set (LIBRARIES "${API_LOWERCASE}-${MAJOR}")
elseif(UNIX)
  set (LIBRARIES "${API_LOWERCASE}")
else()
endif()

if(WIN32)
    # For Windows, since 32-bit and 64-bit items can co-exist, we build each in its own build directory.
    # 32-bit target data goes in build32, and 64-bit target data goes into build.  So, include/link the
    # appropriate data at build time.
    if (CMAKE_CL_64)
        set (BUILDTGT_DIR build)
    else ()
        set (BUILDTGT_DIR build32)
    endif()

    # Use static MSVCRT libraries
    foreach(configuration in CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO
                             CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if(${configuration} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${configuration} "${${configuration}}")
        endif()
    endforeach()

endif()

include_directories(${CMAKE_CURRENT_BINARY_DIR})


if(WIN32)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES")
endif()

include_directories(
    ${DEMO_INCLUDE_DIRS}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_VIDEO_APIS_INCLUDE}/vulkan
    )

######################################################################################


install(TARGETS DESTINATION ${CMAKE_INSTALL_BINDIR})

######################################################################################
# vk-video-dec
if ((${CMAKE_SYSTEM_PROCESSOR} STREQUAL ${CMAKE_HOST_SYSTEM_PROCESSOR}))
    if ((DEMOS_WSI_SELECTION STREQUAL "XCB") OR (DEMOS_WSI_SELECTION STREQUAL "WAYLAND") OR WIN32)
        add_subdirectory(vk-video-dec)
    endif()
endif()
