# The skip-or-fail verdict of encode_device_probe.h, on a host made driverless
# with VK_DRIVER_FILES: device-free, so it gates on every runner. The encoder
# gpu tests take that verdict for every failure before the library has a
# device: 77 only when the probe finds no device that encodes, 1 otherwise.

set(TEST_NAME vk-video-enc-device-probe-test)

add_executable(${TEST_NAME} src/main.cpp)

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${VULKAN_VIDEO_APIS_INCLUDE}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)

target_link_libraries(${TEST_NAME} PRIVATE ${CMAKE_DL_LIBS})

add_test(NAME EncodeDeviceProbeVerdict COMMAND ${TEST_NAME})
set_tests_properties(EncodeDeviceProbeVerdict PROPERTIES LABELS "device-free")
