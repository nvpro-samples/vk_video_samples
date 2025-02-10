set(CMAKE_WARN_DEPRECATED OFF)

set(DEMO_INCLUDE_DIRS
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

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
# vk-video-enc
if ((${CMAKE_SYSTEM_PROCESSOR} STREQUAL ${CMAKE_HOST_SYSTEM_PROCESSOR}))
    if ((DEMOS_WSI_SELECTION STREQUAL "XCB") OR (DEMOS_WSI_SELECTION STREQUAL "WAYLAND") OR WIN32)
        add_subdirectory(vk-video-enc)
    endif()
endif()
