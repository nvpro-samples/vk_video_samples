# Drain() leaves a session usable; Finish() ends it.
#
# Drives the encoder interface only -- no internal header, no library probe seam --
# because the property under test is one the interface promises and a caller
# can observe. Skips (exit 77) only when the device probe finds no device that
# encodes H.264; a session the library fails to open on a host that has one is
# a failure.

set(TEST_NAME vk-video-enc-drain-test)

add_executable(${TEST_NAME} src/main.cpp)

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
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
