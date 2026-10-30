# The video session parameters objects a decode keeps alive.
#
# The layer built here counts each device's video session parameters objects;
# session_parameters_test.py decodes H.264, H.265 and AV1 streams of 100 key
# frames, each with its parameter sets, under it, and a decode must keep at
# most a few objects alive at once and none once the device is destroyed. The
# streams come from ffmpeg (libx264, libx265, libaom-av1); a codec it cannot
# encode is skipped. The layer reaches the decoder through VK_ADD_LAYER_PATH
# and VK_INSTANCE_LAYERS, which the script sets. Linux only: the layer exports
# its entry points the ELF way.

set(LAYER_TARGET VkLayer_vvs_session_parameters_count)

add_library(${LAYER_TARGET} MODULE layer/session_parameters_count_layer.cpp)
target_include_directories(${LAYER_TARGET} PRIVATE
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIR}
)
set_target_properties(${LAYER_TARGET} PROPERTIES CXX_VISIBILITY_PRESET hidden)

# The manifest names the library by its absolute path, so the directory it is
# in is all the loader needs.
set(LAYER_MANIFEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/layer-manifest)
file(GENERATE
     OUTPUT ${LAYER_MANIFEST_DIR}/${LAYER_TARGET}.json
     CONTENT "{
    \"file_format_version\": \"1.1.2\",
    \"layer\": {
        \"name\": \"VK_LAYER_VVS_session_parameters_count\",
        \"type\": \"INSTANCE\",
        \"library_path\": \"$<TARGET_FILE:${LAYER_TARGET}>\",
        \"api_version\": \"1.4.0\",
        \"implementation_version\": \"1\",
        \"description\": \"Test layer: counts the live video session parameters objects of each device\",
        \"functions\": {
            \"vkGetInstanceProcAddr\": \"CountGetInstanceProcAddr\",
            \"vkGetDeviceProcAddr\": \"CountGetDeviceProcAddr\"
        }
    }
}
")

add_test(NAME DecoderSessionParametersBounded
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/session_parameters_test.py
                 --decoder $<TARGET_FILE:vulkan-video-dec-test>
                 --layer-dir ${LAYER_MANIFEST_DIR}
                 --output-dir ${CMAKE_CURRENT_BINARY_DIR}/output)
set_tests_properties(DecoderSessionParametersBounded PROPERTIES
                     SKIP_RETURN_CODE 77 LABELS "gpu")
if(VVS_REQUIRE_GPU)
    # The script reads the environment; the C++ tests get the option as a definition.
    set_tests_properties(DecoderSessionParametersBounded PROPERTIES ENVIRONMENT "VVS_REQUIRE_GPU=1")
endif()
