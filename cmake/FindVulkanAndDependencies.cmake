include(${CMAKE_CURRENT_LIST_DIR}/VvsFetchReport.cmake)

# The Vulkan headers, at a release: a fetch of a branch would build two checkouts of one commit of
# this tree against different headers.
FetchContent_Declare(
    vulkan-headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.365
)
FetchContent_MakeAvailable(vulkan-headers)
vvs_report_fetched(vulkan-headers)

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
endif()

# Find Vulkan SDK
find_package(Vulkan QUIET)

# Vulkan::Vulkan's include directory is the Vulkan_INCLUDE_DIR set above, the fetched headers. As an
# imported target's, it would reach the targets that link Vulkan::Vulkan as a system include
# directory, which compilers search after all the others, a Vulkan SDK's include directory among them
# (cmake/VulkanShaderCompilerBackend.cmake adds it for the SDK's glslang). SYSTEM needs CMake 3.25.
if(TARGET Vulkan::Vulkan)
    set_target_properties(Vulkan::Vulkan PROPERTIES SYSTEM OFF)
endif()

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

# The Vulkan library is the system's: every binary loads it at run time (VK_NO_PROTOTYPES), and the
# headers above are new enough for any loader that takes them.
get_filename_component(VULKAN_LIB_DIR "${Vulkan_LIBRARIES}" DIRECTORY)

include(${CMAKE_CURRENT_LIST_DIR}/VulkanShaderCompilerBackend.cmake)
