# The name of our project is "VULKAN_VIDEO_TESTS". CMakeLists files in this project can
# refer to the root source directory of the project as ${VULKAN_VIDEO_TESTS_SOURCE_DIR} and
# to the root binary directory of the project as ${VULKAN_BINARY_DIR}.
cmake_minimum_required(VERSION 2.8.11)





add_dependencies(VkVideoUtils generate_helper_files)
target_include_directories(
    VkVideoUtils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan
    ${CMAKE_CURRENT_BINARY_DIR}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    )

add_definitions(-DAPI_NAME="${API_NAME}")



if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs")
    add_subdirectory(libs)
else()
    install(DIRECTORY "${LIBVKVIDEOENCODER_BINARY_ROOT}/"
            DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            PATTERN "*.def" EXCLUDE
            PATTERN "*.a" EXCLUDE)
endif()

add_subdirectory(test/vulkan-video-enc)

if(BUILD_DEMOS AND NOT DEFINED DEQP_TARGET)
    add_subdirectory(demos)
endif()
