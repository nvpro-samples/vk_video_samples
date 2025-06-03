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

# Now check if we need to build shaderc and its dependencies
set(USE_SYSTEM_SHADERC ON CACHE BOOL "Use system installed shaderc" FORCE)
option(USE_SYSTEM_SHADERC "Use system installed shaderc" ON)

if(USE_SYSTEM_VULKAN AND USE_SYSTEM_SHADERC)
    if(WIN32)
        # Try to find shaderc in Vulkan SDK Bin directory
        if(DEFINED ENV{VULKAN_SDK})
            # Normalize path
            file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK_PATH)

            # Look in the SDK's Bin directory
            find_library(SHADERC_SHARED_LIBRARY NAMES shaderc_shared
                         PATHS "${VULKAN_SDK_PATH}/lib"
                         NO_DEFAULT_PATH)

            if(SHADERC_SHARED_LIBRARY)
                message(STATUS "Found shaderc at: ${SHADERC_SHARED_LIBRARY}")
                set(shaderc_FOUND TRUE)
                # Store the Bin directory for later use
                get_filename_component(VULKAN_SDK_BIN_DIR "${SHADERC_SHARED_LIBRARY}" DIRECTORY)
                message(STATUS "Vulkan SDK Bin directory: ${VULKAN_SDK_BIN_DIR}")
            else()
                message(STATUS "Could not find shaderc_shared.dll in ${VULKAN_SDK_PATH}/Bin")
            endif()
        endif()
    else()
        find_library(SHADERC_SHARED_LIBRARY NAMES shaderc_shared shaderc)

        if(SHADERC_SHARED_LIBRARY)
            message(STATUS "Found shaderc at: ${SHADERC_SHARED_LIBRARY}")
            set(shaderc_FOUND TRUE)
            # Store the Bin directory for later use
            get_filename_component(VULKAN_SDK_BIN_DIR "${SHADERC_SHARED_LIBRARY}" DIRECTORY)
            message(STATUS "Vulkan SDK Bin directory: ${VULKAN_SDK_BIN_DIR}")
        else()
            message(STATUS "Could not find libshaderc_shared.so in filesystem")
	endif()
    endif()

    if(shaderc_FOUND)
        message(STATUS "Found system shaderc")
    else()
        message(STATUS "System shaderc not found")
        if(WIN32)
            message(STATUS "Make sure Vulkan SDK is installed and VULKAN_SDK environment variable is set")
        endif()
    endif()
endif()

if(USE_SYSTEM_SHADERC AND shaderc_FOUND)
    message(STATUS "Using system shaderc")
    set(SHADERC_LIB "")
else()
    set(SHADERC_LIB "shaderc_shared" CACHE PATH "The name of the shaderc library target decoder/encoder are using." FORCE)
    message(STATUS "Building shaderc and dependencies from source")

    # Fetch SPIRV-Headers first (needed by SPIRV-Tools)
    FetchContent_Declare(
        spirv-headers
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(spirv-headers)

    # Fetch SPIRV-Tools (required for Shaderc)
    FetchContent_Declare(
        spirv-tools
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
        GIT_TAG main
    )
    FetchContent_GetProperties(spirv-tools)
    if(NOT spirv-tools_POPULATED)
        FetchContent_Populate(spirv-tools)
        # SPIRV-Tools settings
        set(SPIRV_SKIP_TESTS ON CACHE BOOL "Disable SPIRV-Tools tests" FORCE)
        set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "Disable SPIRV-Tools executables" FORCE)
        set(SPIRV_BUILD_SHARED ON CACHE BOOL "Build shared SPIRV-Tools" FORCE)
        set(SPIRV_USE_STATIC_LIBS OFF CACHE BOOL "Use dynamic CRT for SPIRV-Tools" FORCE)
        set(SPIRV_TOOLS_INSTALL_EMACS_HELPERS OFF CACHE BOOL "Skip emacs helpers" FORCE)
        set(SPIRV_TOOLS_BUILD_STATIC OFF CACHE BOOL "Build static SPIRV-Tools" FORCE)
        set(SPIRV_TOOLS_BUILD_SHARED ON CACHE BOOL "Build shared SPIRV-Tools" FORCE)
        add_subdirectory(${spirv-tools_SOURCE_DIR} ${spirv-tools_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

	set(SPIRV_TOOLS_LIBRARY_DIR "${CMAKE_BINARY_DIR}/_deps/spirv-tools-build/lib")
    # Ensure the linker knows where to find these libraries
    link_directories(${SPIRV_TOOLS_LIBRARY_DIR})

    # Fetch GLSLang (required for Shaderc)
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG main
    )
    FetchContent_GetProperties(glslang)
    if(NOT glslang_POPULATED)
        FetchContent_Populate(glslang)

        # GLSLang settings
        set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "Disable GLSLang binaries" FORCE)
        set(ENABLE_SPVREMAPPER ON CACHE BOOL "Disable SPVREMAPPER" FORCE)
        set(ENABLE_GLSLANG_JS OFF CACHE BOOL "Disable JavaScript" FORCE)
        set(ENABLE_GLSLANG_WEBMIN OFF CACHE BOOL "Disable WebMin" FORCE)
        set(ENABLE_GLSLANG_WEB OFF CACHE BOOL "Disable Web" FORCE)
        set(ENABLE_GLSLANG_WEB_DEVEL OFF CACHE BOOL "Disable Web Development" FORCE)
        set(ENABLE_HLSL ON CACHE BOOL "Enable HLSL" FORCE)

        # Configure glslang to find SPIRV-Tools
        set(ENABLE_OPT ON CACHE BOOL "Enable SPIRV-Tools optimizer" FORCE)
        set(ALLOW_EXTERNAL_SPIRV_TOOLS ON CACHE BOOL "Allow external SPIRV-Tools" FORCE)
        set(SPIRV_TOOLS_BINARY_ROOT "${spirv-tools_BINARY_DIR}" CACHE PATH "SPIRV-Tools binary root" FORCE)
        set(SPIRV_TOOLS_OPT_LIBRARY_PATH "${spirv-tools_BINARY_DIR}/source/opt" CACHE PATH "SPIRV-Tools opt library path" FORCE)

        add_subdirectory(${glslang_SOURCE_DIR} ${glslang_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

	set(GLSLANG_LIBRARY_DIR "${CMAKE_BINARY_DIR}/_deps/glslang-build")
    # Ensure the linker knows where to find these libraries
    link_directories(${GLSLANG_LIBRARY_DIR})

    # Fetch shaderc
    FetchContent_Declare(
        shaderc
        GIT_REPOSITORY https://github.com/google/shaderc.git
        GIT_TAG v2024.4  # Use the latest stable release or specify a particular commit
    )
    FetchContent_GetProperties(shaderc)
    if(NOT shaderc_POPULATED)
        FetchContent_Populate(shaderc)
        # Ensure shaderc uses our existing glslang and SPIRV-Tools
        set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "Skip examples" FORCE)
        set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "Disable Shaderc copyright check" FORCE)
        set(SHADERC_SKIP_TESTS ON CACHE BOOL "Skip tests" FORCE)
        set(SHADERC_ENABLE_SHARED_CRT ON CACHE BOOL "Use shared CRT" FORCE)
        set(SHADERC_STATIC_CRT OFF CACHE BOOL "Don't use static CRT" FORCE)

        # Enable glslc build explicitly
        set(SHADERC_SKIP_INSTALL OFF CACHE BOOL "Don't skip installation" FORCE)
        set(SHADERC_ENABLE_GLSLC ON CACHE BOOL "Enable glslc" FORCE)
        set(SHADERC_ENABLE_INSTALL ON CACHE BOOL "Enable install" FORCE)

        # Point shaderc to our dependencies
        set(glslang_DIR ${glslang_BINARY_DIR} CACHE PATH "Path to glslang" FORCE)
        set(SPIRV-Tools_DIR ${spirv-tools_BINARY_DIR} CACHE PATH "Path to SPIRV-Tools" FORCE)

        # Tell shaderc to use the external projects we've already built
        set(SHADERC_SPIRV_TOOLS_DIR ${spirv-tools_SOURCE_DIR} CACHE PATH "Source directory for SPIRV-Tools" FORCE)
        set(SHADERC_GLSLANG_DIR ${glslang_SOURCE_DIR} CACHE PATH "Source directory for glslang" FORCE)
        set(SHADERC_THIRD_PARTY_ROOT_DIR ${shaderc_SOURCE_DIR}/third_party CACHE PATH "Root location of shaderc third party dependencies" FORCE)

        # Disable shaderc's built-in handling of third-party deps
        set(SHADERC_SKIP_THIRD_PARTY_BUILD ON CACHE BOOL "Skip building third party dependencies" FORCE)

        add_subdirectory(${shaderc_SOURCE_DIR} ${shaderc_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

    set(SHADERC_LIBRARY_DIR "${CMAKE_BINARY_DIR}/_deps/shaderc-build/libshaderc")
    # Ensure the linker knows where to find these libraries
    link_directories(${SHADERC_LIBRARY_DIR})

    set(SHADERC_SHARED_LIBRARY ${SHADERC_LIB})

    # Set explicit dependencies
    if(TARGET glslang)
        add_dependencies(glslang SPIRV-Tools)
    endif()
    if(TARGET SPIRV-Tools-opt)
        add_dependencies(SPIRV-Tools-opt SPIRV-Tools)
    endif()
    if(TARGET shaderc_shared)
        add_dependencies(shaderc_shared glslang SPIRV-Tools)
    endif()

    find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.h PATHS "${CMAKE_BINARY_DIR}/_deps/shaderc-src/libshaderc/include" NO_DEFAULT_PATH)
    message(STATUS "shaderc inlcude directory: " ${SHADERC_INCLUDE_DIR})
    include_directories(${SHADERC_INCLUDE_DIR})

    # After all the FetchContent_MakeAvailable calls and dependencies setup, add:
    if(WIN32)
        # Install Shaderc
        install(DIRECTORY "${shaderc_BINARY_DIR}/libshaderc/$<CONFIG>/"
                DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
                FILES_MATCHING
                PATTERN "*.lib"
                PATTERN "*.dll")
        install(DIRECTORY "${shaderc_BINARY_DIR}/libshaderc/$<CONFIG>/"
                DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
                FILES_MATCHING
                PATTERN "*.dll")
        # Install glslc
        install(DIRECTORY "${shaderc_BINARY_DIR}/glslc/$<CONFIG>/"
                DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
                FILES_MATCHING
                PATTERN "glslc.exe")
    endif()
endif()

# Add after finding Vulkan SDK
# Find Shaderc
if(DEFINED ENV{VULKAN_SDK})
    # Try to find shaderc in Vulkan SDK first
    find_path(SHADERC_INCLUDE_DIR
        NAMES shaderc/shaderc.h
        PATHS
        "$ENV{VULKAN_SDK}/Include"
        "$ENV{VULKAN_SDK}/include"
        NO_DEFAULT_PATH
    )

    find_library(SHADERC_LIBRARY
        NAMES shaderc_combined
        PATHS
        "$ENV{VULKAN_SDK}/Lib"
        "$ENV{VULKAN_SDK}/lib"
        NO_DEFAULT_PATH
    )
endif()

# If not found in SDK, try system paths
if(NOT SHADERC_INCLUDE_DIR)
    find_path(SHADERC_INCLUDE_DIR
        NAMES shaderc/shaderc.h
    )
endif()

if(NOT SHADERC_LIBRARY)
    find_library(SHADERC_LIBRARY
        NAMES shaderc_combined
    )
endif()

# If still not found, build from source
if(NOT SHADERC_INCLUDE_DIR OR NOT SHADERC_LIBRARY)
    message(STATUS "Shaderc not found in SDK or system, building from source...")
    include(FetchContent)
    FetchContent_Declare(
        shaderc
        GIT_REPOSITORY https://github.com/google/shaderc
        GIT_TAG main
    )

    set(SHADERC_SKIP_TESTS ON)
    set(SHADERC_SKIP_EXAMPLES ON)
    set(SHADERC_SKIP_COPYRIGHT_CHECK ON)

    FetchContent_MakeAvailable(shaderc)

    set(SHADERC_INCLUDE_DIR ${shaderc_SOURCE_DIR}/libshaderc/include)
    set(SHADERC_LIBRARY shaderc)
endif()

if(SHADERC_INCLUDE_DIR AND SHADERC_LIBRARY)
    message(STATUS "Found Shaderc: ${SHADERC_LIBRARY}")
    message(STATUS "Shaderc include: ${SHADERC_INCLUDE_DIR}")
    include_directories(${SHADERC_INCLUDE_DIR})
else()
    message(FATAL_ERROR "Could not find or build Shaderc")
endif()
