# Shader-compiler backend selection for VulkanShaderCompiler.
#
# Included by this project and by external projects that embed VkCodecUtils
# sources (the AQ library, ThreadedRenderingVk), so all of them resolve the
# backend the same way:
#
#   include(${VULKAN_VIDEO_SAMPLES_ROOT}/cmake/VulkanShaderCompilerBackend.cmake)
#   target_sources(mytarget PRIVATE
#       ${VK_CODEC_UTILS_DIR}/VulkanShaderCompiler.cpp
#       ${VK_CODEC_UTILS_DIR}/${VK_SHADER_COMPILER_BACKEND_SOURCE})
#   target_link_libraries(mytarget PRIVATE ${VK_SHADER_COMPILER_LIBS})

include_guard(GLOBAL)

# ----------------------------------------------------------------------------
# GLSL shader compiler backend
#
# VulkanShaderCompiler compiles the runtime-generated GLSL through a backend
# selected here. glslang is the default: it is the compiler shaderc itself
# wraps, so going straight to it drops a dependency without changing what
# compiles the shaders. shaderc stays available for embedders already carrying
# it.
#
# Exported for the targets below and for projects that embed VkCodecUtils
# sources (the AQ library, ThreadedRenderingVk):
#   VK_SHADER_COMPILER_LIBS            libraries to link
#   VK_SHADER_COMPILER_BACKEND_SOURCE  backend implementation to compile
# ----------------------------------------------------------------------------
set(VK_VIDEO_SAMPLES_SHADER_BACKEND "glslang" CACHE STRING
    "GLSL-to-SPIR-V compiler backend for VulkanShaderCompiler: glslang or shaderc")
set_property(CACHE VK_VIDEO_SAMPLES_SHADER_BACKEND PROPERTY STRINGS glslang shaderc)

if(VK_VIDEO_SAMPLES_SHADER_BACKEND STREQUAL "glslang")

    # Prefer the shared glslang, which is what shaderc was (libshaderc_shared)
    # and what browser embedders link. Plain find_library resolves the shared
    # library ahead of the archive; glslang's CONFIG package points at the
    # static archives and pulls SPIRV-Tools in transitively, adding ~8 MB to
    # every binary that compiles the shader compiler. The CONFIG package stays
    # as the fallback for SDK layouts that ship no bare libraries.
    option(VK_VIDEO_SAMPLES_GLSLANG_STATIC
           "Link glslang statically via its CMake package instead of the shared library" OFF)

    # Build glslang from source unconditionally, skipping discovery. Off by
    # default: a system glslang is preferred when there is one. Turn it on for
    # reproducible builds that must not depend on what the machine happens to
    # have, and to exercise the from-source path on a machine that does have
    # glslang installed.
    option(VK_VIDEO_SAMPLES_BUILD_GLSLANG
           "Always build glslang from source instead of searching for it" OFF)

    # Distributions put glslang on the default search path; the Vulkan SDK does
    # not, so hint at it explicitly. The SDK spells the directories Lib/Include
    # on Windows and lib/include elsewhere.
    set(GLSLANG_SDK_LIB_HINTS "")
    set(GLSLANG_SDK_INCLUDE_HINTS "")
    if(DEFINED ENV{VULKAN_SDK})
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" GLSLANG_VULKAN_SDK)
        list(APPEND GLSLANG_SDK_LIB_HINTS "${GLSLANG_VULKAN_SDK}/Lib" "${GLSLANG_VULKAN_SDK}/lib")
        list(APPEND GLSLANG_SDK_INCLUDE_HINTS "${GLSLANG_VULKAN_SDK}/Include" "${GLSLANG_VULKAN_SDK}/include")
    endif()

    if(NOT VK_VIDEO_SAMPLES_GLSLANG_STATIC AND NOT VK_VIDEO_SAMPLES_BUILD_GLSLANG)
        find_library(GLSLANG_LIBRARY NAMES glslang HINTS ${GLSLANG_SDK_LIB_HINTS})
        find_library(GLSLANG_RESOURCE_LIMITS_LIBRARY NAMES glslang-default-resource-limits
                     HINTS ${GLSLANG_SDK_LIB_HINTS})
        find_path(GLSLANG_INCLUDE_DIR NAMES glslang/Public/ShaderLang.h
                  HINTS ${GLSLANG_SDK_INCLUDE_HINTS})

        # MSVC cannot mix a release-CRT import library into a debug build, and
        # the SDK ships the debug halves under a "d" suffix.
        if(MSVC)
            find_library(GLSLANG_LIBRARY_DEBUG NAMES glslangd HINTS ${GLSLANG_SDK_LIB_HINTS})
            find_library(GLSLANG_RESOURCE_LIMITS_LIBRARY_DEBUG NAMES glslang-default-resource-limitsd
                         HINTS ${GLSLANG_SDK_LIB_HINTS})

            # The SDK's glslang is a static library built with ENABLE_OPT=ON, so
            # it carries unresolved SPIRV-Tools references (spvContextCreate,
            # spvValidatorOptions*) that the linker must satisfy even though we
            # never enable the optimizer. The Linux shared library resolves them
            # internally and needs none of this.
            find_library(SPIRV_TOOLS_LIBRARY NAMES SPIRV-Tools HINTS ${GLSLANG_SDK_LIB_HINTS})
            find_library(SPIRV_TOOLS_OPT_LIBRARY NAMES SPIRV-Tools-opt HINTS ${GLSLANG_SDK_LIB_HINTS})
            find_library(SPIRV_TOOLS_LIBRARY_DEBUG NAMES SPIRV-Toolsd HINTS ${GLSLANG_SDK_LIB_HINTS})
            find_library(SPIRV_TOOLS_OPT_LIBRARY_DEBUG NAMES SPIRV-Tools-optd HINTS ${GLSLANG_SDK_LIB_HINTS})
        endif()
    endif()

    if(GLSLANG_LIBRARY AND GLSLANG_RESOURCE_LIMITS_LIBRARY AND GLSLANG_INCLUDE_DIR)
        # GetDefaultResources() lives in glslang-default-resource-limits and is
        # mandatory when parsing. There is deliberately no SPIRV library here:
        # glslang 14 merged GlslangToSpv into libglslang, and the leftover
        # libSPIRV is a stub - 0 exported symbols on Linux, a 2 KB import
        # library in the Windows SDK.
        if(MSVC AND GLSLANG_LIBRARY_DEBUG AND GLSLANG_RESOURCE_LIMITS_LIBRARY_DEBUG)
            set(VK_SHADER_COMPILER_LIBS
                optimized ${GLSLANG_LIBRARY}                debug ${GLSLANG_LIBRARY_DEBUG}
                optimized ${GLSLANG_RESOURCE_LIMITS_LIBRARY} debug ${GLSLANG_RESOURCE_LIMITS_LIBRARY_DEBUG})
            if(SPIRV_TOOLS_LIBRARY AND SPIRV_TOOLS_OPT_LIBRARY)
                list(APPEND VK_SHADER_COMPILER_LIBS
                     optimized ${SPIRV_TOOLS_OPT_LIBRARY} optimized ${SPIRV_TOOLS_LIBRARY})
                if(SPIRV_TOOLS_LIBRARY_DEBUG AND SPIRV_TOOLS_OPT_LIBRARY_DEBUG)
                    list(APPEND VK_SHADER_COMPILER_LIBS
                         debug ${SPIRV_TOOLS_OPT_LIBRARY_DEBUG} debug ${SPIRV_TOOLS_LIBRARY_DEBUG})
                endif()
                message(STATUS "Found SPIRV-Tools for static glslang: ${SPIRV_TOOLS_LIBRARY}")
            endif()
            message(STATUS "Found glslang: ${GLSLANG_LIBRARY} (debug: ${GLSLANG_LIBRARY_DEBUG})")
        else()
            set(VK_SHADER_COMPILER_LIBS ${GLSLANG_LIBRARY} ${GLSLANG_RESOURCE_LIMITS_LIBRARY})
            message(STATUS "Found glslang: ${GLSLANG_LIBRARY}")
        endif()
        # Both the include root and its glslang/ subdirectory: the sources spell
        # the SPIRV headers <SPIRV/...>, which an installed layout keeps under
        # include/glslang/ while a source tree keeps at the top level.
        include_directories(${GLSLANG_INCLUDE_DIR} ${GLSLANG_INCLUDE_DIR}/glslang)
        message(STATUS "Found glslang resource limits: ${GLSLANG_RESOURCE_LIMITS_LIBRARY}")
    else()
        # The SDK nests its package one level deeper than the standard
        # <prefix>/lib/cmake/<name> layout, so point at it directly. Skipped
        # entirely when the from-source build was asked for, so that the option
        # still reaches the fetch on a machine that has glslang installed.
        if(NOT VK_VIDEO_SAMPLES_BUILD_GLSLANG)
            find_package(glslang CONFIG QUIET
                         PATHS "${GLSLANG_VULKAN_SDK}/Lib/cmake/glslang/glslang"
                               "${GLSLANG_VULKAN_SDK}/lib/cmake/glslang/glslang"
                               "${GLSLANG_VULKAN_SDK}/Lib/cmake/glslang"
                               "${GLSLANG_VULKAN_SDK}/lib/cmake/glslang")
        endif()
        if(TARGET glslang::glslang AND TARGET glslang::glslang-default-resource-limits)
            set(VK_SHADER_COMPILER_LIBS glslang::glslang glslang::glslang-default-resource-limits)
            find_path(GLSLANG_INCLUDE_DIR NAMES glslang/Public/ShaderLang.h
                      HINTS ${GLSLANG_SDK_INCLUDE_HINTS})
            if(GLSLANG_INCLUDE_DIR)
                include_directories(${GLSLANG_INCLUDE_DIR} ${GLSLANG_INCLUDE_DIR}/glslang)
            endif()
            message(STATUS "Found glslang (CONFIG package)")
        else()
            # Nothing installed: build glslang from source, the way the tree
            # used to build shaderc and its dependency chain. This is what keeps
            # a bare CI runner - no glslang package, no Vulkan SDK - able to
            # configure. Only glslang is needed, not the four-project chain
            # shaderc required: shaderc pulled in SPIRV-Tools for its
            # optimisation pass, and we compile with disableOptimizer = true.
            # ENABLE_OPT=OFF therefore drops SPIRV-Headers and SPIRV-Tools
            # entirely.
            message(STATUS "glslang not found - building it from source")

            include(FetchContent)
            set(ENABLE_OPT              OFF CACHE BOOL "No SPIRV-Tools optimiser: we disable it at runtime" FORCE)
            set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "No glslangValidator/spirv-remap binaries" FORCE)
            set(ENABLE_SPVREMAPPER      OFF CACHE BOOL "No SPIRV remapper" FORCE)
            set(ENABLE_GLSLANG_JS       OFF CACHE BOOL "No JavaScript bindings" FORCE)
            set(GLSLANG_TESTS           OFF CACHE BOOL "No glslang tests" FORCE)
            set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "Do not install glslang with this project" FORCE)
            set(BUILD_EXTERNAL          OFF CACHE BOOL "No External/ subprojects (gtest)" FORCE)

            FetchContent_Declare(
                glslang
                GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
                GIT_TAG main
                GIT_SHALLOW TRUE
                EXCLUDE_FROM_ALL
            )
            FetchContent_MakeAvailable(glslang)

            if(NOT TARGET glslang OR NOT TARGET glslang-default-resource-limits)
                message(FATAL_ERROR
                    "glslang was fetched but did not provide the expected targets "
                    "(glslang, glslang-default-resource-limits). Install the glslang "
                    "development package (Ubuntu/Debian: glslang-dev) or the Vulkan SDK, "
                    "or configure with -DVK_VIDEO_SAMPLES_SHADER_BACKEND=shaderc.")
            endif()

            # In the source tree glslang/ holds Public/ and Include/ while SPIRV/
            # sits beside it, so the source directory itself is the include root
            # that satisfies both <glslang/Public/...> and <SPIRV/...>.
            set(VK_SHADER_COMPILER_LIBS glslang glslang-default-resource-limits)
            include_directories(${glslang_SOURCE_DIR})
            message(STATUS "Built glslang from source: ${glslang_SOURCE_DIR}")
        endif()
    endif()

    set(VK_SHADER_COMPILER_BACKEND_SOURCE VulkanShaderCompilerGlslang.cpp)

elseif(VK_VIDEO_SAMPLES_SHADER_BACKEND STREQUAL "shaderc")

    # Same Vulkan SDK hints as the glslang branch: the SDK is not on the
    # default search path. shaderc_shared is preferred because it is an import
    # library for the DLL, which avoids the static-library STL-version coupling
    # that shaderc_combined has on MSVC.
    set(SHADERC_SDK_LIB_HINTS "")
    set(SHADERC_SDK_INCLUDE_HINTS "")
    if(DEFINED ENV{VULKAN_SDK})
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" SHADERC_VULKAN_SDK)
        list(APPEND SHADERC_SDK_LIB_HINTS "${SHADERC_VULKAN_SDK}/Lib" "${SHADERC_VULKAN_SDK}/lib")
        list(APPEND SHADERC_SDK_INCLUDE_HINTS "${SHADERC_VULKAN_SDK}/Include" "${SHADERC_VULKAN_SDK}/include")
    endif()

    find_library(SHADERC_SHARED_LIBRARY NAMES shaderc_shared shaderc HINTS ${SHADERC_SDK_LIB_HINTS})
    find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.h HINTS ${SHADERC_SDK_INCLUDE_HINTS})

    if(SHADERC_SHARED_LIBRARY AND SHADERC_INCLUDE_DIR)
        set(VK_SHADER_COMPILER_LIBS ${SHADERC_SHARED_LIBRARY})
        include_directories(${SHADERC_INCLUDE_DIR})
        message(STATUS "Found shaderc: ${SHADERC_SHARED_LIBRARY}")
    else()
        # Nothing installed: build shaderc and its dependencies from source.
        # shaderc genuinely needs the whole chain - SPIRV-Headers feeds
        # SPIRV-Tools, SPIRV-Tools feeds glslang's optimiser, and shaderc sits
        # on top - which is why the default glslang backend is the cheaper one
        # to bootstrap.
        message(STATUS "shaderc not found - building it and its dependencies from source")

        include(FetchContent)

        FetchContent_Declare(
            spirv-headers
            GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(spirv-headers)

        set(SPIRV_SKIP_TESTS       ON  CACHE BOOL "Disable SPIRV-Tools tests" FORCE)
        set(SPIRV_SKIP_EXECUTABLES ON  CACHE BOOL "Disable SPIRV-Tools executables" FORCE)
        set(SPIRV_USE_STATIC_LIBS  OFF CACHE BOOL "Use dynamic CRT for SPIRV-Tools" FORCE)
        FetchContent_Declare(
            spirv-tools
            GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(spirv-tools)

        # shaderc drives glslang's optimiser, so unlike the glslang backend this
        # chain does need ENABLE_OPT and an external SPIRV-Tools.
        set(ENABLE_OPT                 ON  CACHE BOOL "shaderc needs glslang's SPIRV-Tools optimiser" FORCE)
        set(ALLOW_EXTERNAL_SPIRV_TOOLS ON  CACHE BOOL "Use the SPIRV-Tools fetched above" FORCE)
        set(ENABLE_GLSLANG_BINARIES    OFF CACHE BOOL "No glslangValidator binary" FORCE)
        set(GLSLANG_TESTS              OFF CACHE BOOL "No glslang tests" FORCE)
        set(GLSLANG_ENABLE_INSTALL     OFF CACHE BOOL "Do not install glslang with this project" FORCE)
        set(BUILD_EXTERNAL             OFF CACHE BOOL "No External/ subprojects (gtest)" FORCE)
        FetchContent_Declare(
            glslang
            GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(glslang)

        set(SHADERC_SKIP_TESTS           ON  CACHE BOOL "Skip shaderc tests" FORCE)
        set(SHADERC_SKIP_EXAMPLES        ON  CACHE BOOL "Skip shaderc examples" FORCE)
        set(SHADERC_SKIP_COPYRIGHT_CHECK ON  CACHE BOOL "Skip shaderc copyright check" FORCE)
        set(SHADERC_SKIP_INSTALL         ON  CACHE BOOL "Do not install shaderc with this project" FORCE)
        set(SHADERC_ENABLE_SHARED_CRT    ON  CACHE BOOL "Use the shared CRT on MSVC" FORCE)
        set(SHADERC_SKIP_THIRD_PARTY_BUILD ON CACHE BOOL "Use the dependencies fetched above" FORCE)
        set(SHADERC_GLSLANG_DIR     "${glslang_SOURCE_DIR}"     CACHE PATH "glslang source for shaderc" FORCE)
        set(SHADERC_SPIRV_TOOLS_DIR "${spirv-tools_SOURCE_DIR}" CACHE PATH "SPIRV-Tools source for shaderc" FORCE)
        FetchContent_Declare(
            shaderc
            GIT_REPOSITORY https://github.com/google/shaderc.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(shaderc)

        if(TARGET shaderc_shared)
            set(VK_SHADER_COMPILER_LIBS shaderc_shared)
        elseif(TARGET shaderc)
            set(VK_SHADER_COMPILER_LIBS shaderc)
        else()
            message(FATAL_ERROR
                "shaderc was fetched but provided neither a shaderc_shared nor a "
                "shaderc target. Install the shaderc development package or the "
                "Vulkan SDK, or use the default "
                "-DVK_VIDEO_SAMPLES_SHADER_BACKEND=glslang.")
        endif()
        include_directories(${shaderc_SOURCE_DIR}/libshaderc/include)
        message(STATUS "Built shaderc from source: ${shaderc_SOURCE_DIR}")
    endif()

    set(VK_SHADER_COMPILER_BACKEND_SOURCE VulkanShaderCompilerShaderc.cpp)

else()
    message(FATAL_ERROR
        "Unknown VK_VIDEO_SAMPLES_SHADER_BACKEND '${VK_VIDEO_SAMPLES_SHADER_BACKEND}': "
        "expected glslang or shaderc.")
endif()

message(STATUS "Shader compiler backend: ${VK_VIDEO_SAMPLES_SHADER_BACKEND}")

set(VK_SHADER_COMPILER_LIBS "${VK_SHADER_COMPILER_LIBS}" CACHE INTERNAL
    "Libraries providing the VulkanShaderCompiler backend")
set(VK_SHADER_COMPILER_BACKEND_SOURCE "${VK_SHADER_COMPILER_BACKEND_SOURCE}" CACHE INTERNAL
    "VulkanShaderCompiler backend implementation source file")
