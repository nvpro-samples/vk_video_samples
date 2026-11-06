# SYNC_FD fences around encode submissions: a frame waits on a SYNC_FD the
# producer exported, and the semaphore it signals exports a SYNC_FD that
# signals -- on the encode queue (an OPTIMAL image read directly, written by a
# second device through OPAQUE_FD memory) and on the staging queue (a LINEAR
# image).
#
# Drives the encoder interface with an adopted instance, the shape an embedding
# renderer uses. Skips (exit 77) when the device probe finds no device that
# encodes H.264, or when the device cannot export and import SYNC_FD
# semaphores. A release fence that does not signal within 5 s fails the test at
# once rather than hanging it.

set(TEST_NAME vk-video-enc-sync-fd-test)

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
                     TIMEOUT 300
                     SKIP_RETURN_CODE 77)
