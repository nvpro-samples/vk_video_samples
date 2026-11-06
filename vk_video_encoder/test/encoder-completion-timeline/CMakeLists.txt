# The session's completion timeline through ICompletionTimeline: frames with a
# B-frame GOP are submitted, every frame N is waited for at value N + 1 with a
# bounded vkWaitSemaphores and must then become Ready and acquirable, the value
# never decreases, the timeline exports as an OPAQUE_FD that imports into a
# semaphore reading the same payload, and Finish takes the timeline away.
#
# Skips (exit 77) when no device encodes H.264.

set(TEST_NAME vk-video-enc-completion-timeline-test)

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
