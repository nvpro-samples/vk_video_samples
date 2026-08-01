# Vulkan dispatch-table generation for VkCodecUtils.
#
# Included by this project and by external projects that embed VkCodecUtils
# sources (the AQ library, ThreadedRenderingVk), so all of them generate and
# consume the table the same way:
#
#   include(${VULKAN_VIDEO_SAMPLES_ROOT}/cmake/VulkanDispatchTable.cmake)
#   target_sources(mytarget PRIVATE ${VK_DISPATCH_TABLE_SOURCE})
#
# The generated pair used to be written into the SOURCE tree and checked in.
# That made every build rewrite files other targets were compiling: on Windows
# the generator's open fails with a sharing violation while cl.exe holds the
# header (PermissionError -> MSB8066), intermittently and depending only on
# build order. POSIX allows the concurrent write, which is why Linux never saw
# it. Generating into the build directory removes the shared mutable state -
# each build tree owns its copy, nothing writes into the checkout, and parallel
# builds cannot collide.
#
# Exported:
#   VK_DISPATCH_TABLE_SOURCE      generated .cpp to compile
#   VK_DISPATCH_TABLE_HEADER      generated .h
#   VK_DISPATCH_TABLE_INCLUDE_DIR directory to add to the include path

include_guard(GLOBAL)

# Callers that embed VkCodecUtils set VULKAN_VIDEO_SAMPLES_ROOT; inside this
# project the module sits one level below the checkout root.
if(NOT VULKAN_VIDEO_SAMPLES_ROOT)
    get_filename_component(VULKAN_VIDEO_SAMPLES_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

set(VK_DISPATCH_TABLE_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
set(VK_DISPATCH_TABLE_HEADER "${VK_DISPATCH_TABLE_INCLUDE_DIR}/VkCodecUtils/HelpersDispatchTable.h")
set(VK_DISPATCH_TABLE_SOURCE "${VK_DISPATCH_TABLE_INCLUDE_DIR}/VkCodecUtils/HelpersDispatchTable.cpp")

file(MAKE_DIRECTORY "${VK_DISPATCH_TABLE_INCLUDE_DIR}/VkCodecUtils")

set(_vk_dispatch_generator "${VULKAN_VIDEO_SAMPLES_ROOT}/scripts/generate-dispatch-table.py")

add_custom_command(OUTPUT "${VK_DISPATCH_TABLE_HEADER}"
    COMMAND ${Python3_EXECUTABLE} "${_vk_dispatch_generator}" "${VK_DISPATCH_TABLE_HEADER}"
    DEPENDS "${_vk_dispatch_generator}"
    COMMENT "Generating ${VK_DISPATCH_TABLE_HEADER}"
    VERBATIM
)

# The header is a prerequisite of the source, not a sibling of it. Embedders
# list only VK_DISPATCH_TABLE_SOURCE in their target sources, so nothing would
# otherwise request the header and the compile fails on a missing include -
# the two custom commands are only run on demand.
add_custom_command(OUTPUT "${VK_DISPATCH_TABLE_SOURCE}"
    COMMAND ${Python3_EXECUTABLE} "${_vk_dispatch_generator}" "${VK_DISPATCH_TABLE_SOURCE}"
    DEPENDS "${_vk_dispatch_generator}" "${VK_DISPATCH_TABLE_HEADER}"
    COMMENT "Generating ${VK_DISPATCH_TABLE_SOURCE}"
    VERBATIM
)

# CMP0118: make the GENERATED property visible to targets in other directories,
# so subdirectory targets can list VK_DISPATCH_TABLE_SOURCE without redeclaring
# the custom command.
set_source_files_properties(
    "${VK_DISPATCH_TABLE_HEADER}" "${VK_DISPATCH_TABLE_SOURCE}"
    PROPERTIES GENERATED TRUE)

add_custom_target(GenerateDispatchTables
    DEPENDS "${VK_DISPATCH_TABLE_HEADER}" "${VK_DISPATCH_TABLE_SOURCE}")

# Two entries: sources spell the header both <VkCodecUtils/HelpersDispatchTable.h>
# and, from inside VkCodecUtils, "HelpersDispatchTable.h".
include_directories(
    "${VK_DISPATCH_TABLE_INCLUDE_DIR}"
    "${VK_DISPATCH_TABLE_INCLUDE_DIR}/VkCodecUtils")
