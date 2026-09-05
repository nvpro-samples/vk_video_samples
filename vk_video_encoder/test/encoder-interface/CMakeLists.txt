# The v3 interface, driven the way a host drives it.
#
# Links the encoder statically: the test exercises the interface, and a static
# link keeps it independent of whichever shared library happens to be found
# first on the loader path.

set(TEST_NAME vk-video-enc-interface-test)

add_executable(${TEST_NAME} src/main.cpp)

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
)

target_link_libraries(${TEST_NAME} PRIVATE vkvideo-encoder-static ${CMAKE_DL_LIBS})

if (TARGET GenVulkanDispatchTable)
    add_dependencies(${TEST_NAME} GenVulkanDispatchTable)
endif()

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
# A run that skips every check reports SKIP, not PASS: see Report::Summarise.
set_tests_properties(${TEST_NAME} PROPERTIES
                     LABELS "gpu"
                     SKIP_RETURN_CODE 77)

# The C++17 floor is a claim about the public header, and a claim that nothing
# checks decays the first time someone writes std::span in it. The rest of the
# tree builds at C++20, so compiling this test at C++20 proves nothing about
# the floor; this compiles the header alone, at exactly -std=c++17, and fails
# the build if it needs anything newer.
#
# Syntax-only: there is nothing to link and nothing to run.
set(V3_FLOOR_PROBE ${CMAKE_CURRENT_BINARY_DIR}/v3_cxx17_floor_probe.cpp)
file(WRITE ${V3_FLOOR_PROBE}
     "#include \"vulkan_video_encoder.h\"\nint main() { return 0; }\n")

add_test(NAME vk-video-enc-header-cxx17-floor
         COMMAND ${CMAKE_CXX_COMPILER}
                 -std=c++17 -fsyntax-only
                 -Wall -Wextra -Werror
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
                 -I${VULKAN_VIDEO_APIS_INCLUDE}
                 -I${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
                 -DVK_USE_PLATFORM_XLIB_KHR
                 -DVK_USE_PLATFORM_XCB_KHR
                 -DVK_USE_PLATFORM_WAYLAND_KHR
                 ${V3_FLOOR_PROBE})

# The window-system defines are the point of the probe as much as the standard
# is: Xlib's Success/None/Status macros only bite a consumer that has included
# it, which is every browser and compositor on Linux.
set_tests_properties(vk-video-enc-header-cxx17-floor PROPERTIES LABELS "device-free")
