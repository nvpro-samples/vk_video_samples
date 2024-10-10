# The name of our project is "VULKAN_VIDEO_TESTS". CMakeLists files in this project can
# refer to the root source directory of the project as ${VULKAN_VIDEO_TESTS_SOURCE_DIR} and
# to the root binary directory of the project as ${VULKAN_BINARY_DIR}.
cmake_minimum_required(VERSION 3.0)

include(CheckCXXCompilerFlag)







add_dependencies(VkVideoUtils generate_helper_files)
target_include_directories(
    VkVideoUtils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan
    ${CMAKE_CURRENT_BINARY_DIR}
    ${VULKAN_VIDEO_APIS_INCLUDE}
    )

add_definitions(-DAPI_NAME="${API_NAME}")


if(BUILD_DEMOS)
    add_subdirectory(demos)
endif()
