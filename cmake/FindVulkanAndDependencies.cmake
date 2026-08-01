# Fetch the latest Vulkan headers
FetchContent_Declare(
    vulkan-headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG main
)
FetchContent_MakeAvailable(vulkan-headers)

set(Vulkan_INCLUDE_DIR ${vulkan-headers_SOURCE_DIR}/include CACHE PATH "Path to Vulkan include headers directory" FORCE)
set(VULKAN_HEADERS_INCLUDE_DIR ${vulkan-headers_SOURCE_DIR}/include CACHE PATH "Path to Vulkan local include headers directory" FORCE)

message(STATUS "VULKAN_HEADERS_INCLUDE_DIR: ${VULKAN_HEADERS_INCLUDE_DIR}")

if(EXISTS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan_core.h")
    file(STRINGS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan_core.h" VK_HEADER_VERSION_LINE
         REGEX "^#define VK_HEADER_VERSION ")
    file(STRINGS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan_core.h" VK_HEADER_VERSION_COMPLETE_LINE
         REGEX "^#define VK_HEADER_VERSION_COMPLETE ")

    message(STATUS "Vulkan Header Version Line: ${VK_HEADER_VERSION_LINE}")
    message(STATUS "Vulkan Complete Version Line: ${VK_HEADER_VERSION_COMPLETE_LINE}")

    # Extract version number from VK_HEADER_VERSION
    string(REGEX MATCH "([0-9]+)$" _ ${VK_HEADER_VERSION_LINE})
    set(VK_PATCH_VERSION ${CMAKE_MATCH_1})

    # Extract major and minor version
    file(STRINGS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan_core.h" VK_VERSION_1_3_LINE
         REGEX "^#define VK_API_VERSION_1_3")
    if(VK_VERSION_1_3_LINE)
        set(VK_MAJOR_VERSION 1)
        set(VK_MINOR_VERSION 3)
    endif()

    # Compare versions
    if(VK_MAJOR_VERSION EQUAL 1 AND VK_MINOR_VERSION EQUAL 3 AND VK_PATCH_VERSION LESS 302)
        message(STATUS "System Vulkan SDK version ${VK_MAJOR_VERSION}.${VK_MINOR_VERSION}.${VK_PATCH_VERSION} is too old, will fetch and build required version")
        set(USE_SYSTEM_VULKAN OFF)
    else()
        message(STATUS "Found suitable Vulkan version: ${VK_MAJOR_VERSION}.${VK_MINOR_VERSION}.${VK_PATCH_VERSION}")
        set(USE_SYSTEM_VULKAN ON)
    endif()
else()
    set(USE_SYSTEM_VULKAN OFF)
endif()

# Find Vulkan SDK
find_package(Vulkan QUIET)

if(Vulkan_FOUND)
    # Set Vulkan headers path (we are using the local headers)
    # set(VULKAN_HEADERS_INCLUDE_DIR ${Vulkan_INCLUDE_DIR})

    # Additional Vulkan-related variables
    set(VULKAN_LIBRARIES ${Vulkan_LIBRARIES})

    # Check for required components
    if(NOT EXISTS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan.h")
        message(STATUS "Could not find vulkan.h in ${VULKAN_HEADERS_INCLUDE_DIR}")
    endif()
else()
    message(STATUS "Vulkan SDK not found. Please install Vulkan SDK.")
endif()

# Optional: Find other dependencies like SPIRV-Tools if needed

if(USE_SYSTEM_VULKAN)
    # Use system Vulkan
    message(STATUS "Using system Vulkan SDK")
    get_filename_component(VULKAN_LIB_DIR "${Vulkan_LIBRARIES}" DIRECTORY)
else()
    # Fetch and build our own Vulkan components
    message(STATUS "Building Vulkan components from source")

    # Set Vulkan Loader options to disable tests
    set(BUILD_TESTS OFF CACHE BOOL "Disable Vulkan-Loader tests" FORCE)

    # Fetch the Vulkan Loader
    FetchContent_Declare(
        vulkan-loader
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Loader.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(vulkan-loader)
    set(VULKAN_LOADER_LIBRARY_DIR "${CMAKE_BINARY_DIR}/_deps/vulkan-loader-build/loader")
    link_directories(${VULKAN_LOADER_LIBRARY_DIR})
endif()

include(${CMAKE_CURRENT_LIST_DIR}/VulkanShaderCompilerBackend.cmake)
