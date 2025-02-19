# Windows specific settings
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LIB_ARCH_DIR "wddm2_amd64_debug" CACHE STRING "User defined library target")
else()
    set(LIB_ARCH_DIR "wddm2_amd64_release" CACHE STRING "User defined library target")
endif()

# Windows specific definitions
add_definitions(
    -DVK_USE_PLATFORM_WIN32_KHR
    -DWIN32_LEAN_AND_MEAN
    -DNOMINMAX="1"
)

# Build path decoration options
option(DISABLE_BUILD_PATH_DECORATION "Disable the decoration of the gslang and SPIRV-Tools build path with MSVC build type info" OFF)
option(DISABLE_BUILDTGT_DIR_DECORATION "Disable the decoration of the gslang and SPIRV-Tools build path with target info" OFF)
option(ENABLE_WIN10_ONECORE "Link the loader with OneCore umbrella libraries" OFF)

# Set build directories based on architecture
if(DISABLE_BUILDTGT_DIR_DECORATION)
    set(BUILDTGT_DIR "")
    set(BINDATA_DIR "")
    set(LIBSOURCE_DIR "")
elseif(CMAKE_CL_64)
    set(BUILDTGT_DIR build)
    set(BINDATA_DIR Bin)
    set(LIBSOURCE_DIR Lib)
else()
    set(BUILDTGT_DIR build32)
    set(BINDATA_DIR Bin32)
    set(LIBSOURCE_DIR Lib32)
endif()

# Build path decoration
if(DISABLE_BUILD_PATH_DECORATION)
    set(DEBUG_DECORATION "")
    set(RELEASE_DECORATION "")
else()
    set(DEBUG_DECORATION "Debug")
    set(RELEASE_DECORATION "Release")
endif()

# MSVC specific settings
if(MSVC)
    if(NOT MSVC_VERSION LESS 1900)
        message(STATUS "Building with control flow guard")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/guard:cf>")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /guard:cf")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /guard:cf")
    endif()

    # Warning settings
    add_compile_options(
        "$<$<CXX_COMPILER_ID:MSVC>:/WX>"    # Warnings as errors
        "$<$<CXX_COMPILER_ID:MSVC>:/GR->"   # Disable RTTI
        "$<$<CXX_COMPILER_ID:MSVC>:/w34456>" # Nested declarations
        "$<$<CXX_COMPILER_ID:MSVC>:/w34701>" # Potentially uninitialized vars
        "$<$<CXX_COMPILER_ID:MSVC>:/w34703>"
        "$<$<CXX_COMPILER_ID:MSVC>:/w34057>" # Different indirection types
        "$<$<CXX_COMPILER_ID:MSVC>:/w34245>" # Signed/unsigned mismatch
        "$<$<CXX_COMPILER_ID:MSVC>:/wd4996>" # Deprecated functions
    )

    # Enable multi-processor compilation
    add_compile_options(/MP)

    # Disable specific warnings
    add_compile_options(/wd4251 /wd4275)

    # Enable exception handling
    add_compile_options(/EHsc)
endif()

# Set demo include directories
set(DEMO_INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/icd/common"
    ${DEMO_INCLUDE_DIRS}
)

# Set architecture
if(CMAKE_GENERATOR_PLATFORM)
    set(ARCH ${CMAKE_GENERATOR_PLATFORM})
else()
    set(ARCH "x64")
endif()

# ... other Windows specific settings