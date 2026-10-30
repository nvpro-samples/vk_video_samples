# An encode on a device that states no preferred GOP length.
#
# The layer built here makes every encode quality level state
# preferredGopFrameCount 0, which a device may state and the GPUs the tests run
# on do not; gop_no_preference_test.py encodes H.264, H.265 and AV1 under it
# with no GOP length asked for, and the encoder must give the GOP the codec's
# default length. The layer reaches the encoder through VK_ADD_LAYER_PATH and
# VK_INSTANCE_LAYERS, which the script sets. Linux only: the layer exports its
# entry points the ELF way.

set(LAYER_TARGET VkLayer_vvs_gop_no_preference)

add_library(${LAYER_TARGET} MODULE layer/gop_no_preference_layer.cpp)
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
        \"name\": \"VK_LAYER_VVS_gop_no_preference\",
        \"type\": \"INSTANCE\",
        \"library_path\": \"$<TARGET_FILE:${LAYER_TARGET}>\",
        \"api_version\": \"1.4.0\",
        \"implementation_version\": \"1\",
        \"description\": \"Test layer: every encode quality level states no preferred GOP length\",
        \"functions\": {
            \"vkGetInstanceProcAddr\": \"GopNoPreferenceGetInstanceProcAddr\",
            \"vkGetDeviceProcAddr\": \"GopNoPreferenceGetDeviceProcAddr\"
        }
    }
}
")

add_test(NAME EncoderGopNoPreference
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/gop_no_preference_test.py
                 --encoder $<TARGET_FILE:vulkan-video-enc-test>
                 --layer-dir ${LAYER_MANIFEST_DIR}
                 --output-dir ${CMAKE_CURRENT_BINARY_DIR}/output)
set_tests_properties(EncoderGopNoPreference PROPERTIES
                     SKIP_RETURN_CODE 77 LABELS "gpu")
if(VVS_REQUIRE_GPU)
    # The script reads the environment; the C++ tests get the option as a definition.
    set_tests_properties(EncoderGopNoPreference PROPERTIES ENVIRONMENT "VVS_REQUIRE_GPU=1")
endif()
