# fd-backed images through IFrameSubmitter::SubmitFrame: a second device on the
# session's physical device exports NV12 images as OPAQUE_FD (LINEAR, staged
# copy, Transfer; OPTIMAL, read by the encode, Borrow) and as DMA_BUF (LINEAR,
# or a DRM format modifier the encoder reads), and every frame is submitted by
# its fd alone. Checks that every frame encodes, that Transfer closes the fd on
# success and on refusal and Borrow never does, and that descriptions the
# device cannot import are refused.
#
# Drives the encoder interface with an adopted instance. Skips (exit 77) when no
# device encodes H.264, and reports a case it cannot export or import as SKIP.

set(TEST_NAME vk-video-enc-import-fd-test)

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
