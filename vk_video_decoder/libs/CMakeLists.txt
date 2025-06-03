cmake_minimum_required(VERSION 3.20.0)
project(${VULKAN_VIDEO_DECODER_LIB}, LANGUAGES CXX)

set(LIBVKVIDEODECODER_SRC
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/../src/vulkan_video_decoder.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/Helpers.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/HelpersDispatchTable.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceContext.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanShaderCompiler.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceMemoryImpl.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDeviceMemoryImpl.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanShaderCompiler.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkBufferResource.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkBufferResource.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkImageResource.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkImageResource.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDescriptorSetLayout.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanDescriptorSetLayout.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBuffersSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBuffersSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanComputePipeline.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanComputePipeline.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSession.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoSession.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/FrameProcessor.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoProcessor.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoProcessor.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFrame.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFrame.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/pattern.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/pattern.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoUtils.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanVideoUtils.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilter.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilter.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilterYuvCompute.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFilterYuvCompute.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSamplerYcbcrConversion.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSamplerYcbcrConversion.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFenceSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanFenceSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSemaphoreSet.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanSemaphoreSet.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VkVideoRefCountBase.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/nvVkFormats.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBistreamBufferImpl.h
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanBistreamBufferImpl.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBufferPool.cpp
    ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}/VkCodecUtils/VulkanCommandBufferPool.h
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkDecoderUtils/VideoStreamDemuxer.h
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoDecoder/VkVideoDecoder.cpp
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoParser/VulkanVideoParser.cpp
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoDecoder/VkVideoDecoder.h
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoDecoder/VkParserVideoPictureParameters.h
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VkVideoDecoder/VkParserVideoPictureParameters.cpp
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.h
    ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}/VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.cpp
    )

set(definitions
    PRIVATE -DVK_NO_PROTOTYPES
    PRIVATE -DGLM_FORCE_RADIANS)

set(includes
    PRIVATE ${VK_VIDEO_DECODER_LIBS_SOURCE_ROOT}
    PRIVATE ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

set(libraries PRIVATE ${CMAKE_THREAD_LIBS_INIT})

# Extract the directory part of the Vulkan library path
get_filename_component(VULKAN_LIB_DIR "${Vulkan_LIBRARIES}" DIRECTORY)

link_directories(
    ${VULKAN_VIDEO_DEVICE_LIBS_PATH}
    ${VULKAN_VIDEO_DEC_LIBS_PATH}
    ${VULKAN_VIDEO_PARSER_LIB_PATH}
    ${LIBNVPARSER_BINARY_ROOT}
    ${VULKAN_LIB_DIR}
    )

if(WIN32)
    list(APPEND libraries PRIVATE ${VULKAN_VIDEO_PARSER_LIB} ${SHADERC_SHARED_LIBRARY})
else()
    list(APPEND libraries PRIVATE ${SHADERC_SHARED_LIBRARY})
    list(APPEND libraries PRIVATE -L${CMAKE_INSTALL_LIBDIR} -l${VULKAN_VIDEO_PARSER_LIB})
    list(APPEND libraries PRIVATE -L${LIBNVPARSER_BINARY_ROOT} -l${VULKAN_VIDEO_PARSER_LIB})
endif()

if(WIN32)
    list(APPEND definitions PRIVATE -DVK_USE_PLATFORM_WIN32_KHR)
    list(APPEND definitions PRIVATE -DWIN32_LEAN_AND_MEAN)

elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    list(APPEND libraries PRIVATE -ldl -lrt -lpthread)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)

include_directories(BEFORE "${CMAKE_BINARY_DIR}")

list(APPEND includes PRIVATE ${VK_VIDEO_DECODER_LIBS_INCLUDE_ROOT})
list(APPEND includes PRIVATE ${VK_VIDEO_COMMON_LIBS_SOURCE_ROOT})
list(APPEND includes PRIVATE ${VULKAN_VIDEO_PARSER_INCLUDE})
list(APPEND includes PRIVATE ${VULKAN_VIDEO_APIS_INCLUDE})
list(APPEND includes PRIVATE ${VULKAN_VIDEO_APIS_INCLUDE}/vulkan)
list(APPEND includes PRIVATE ${VULKAN_VIDEO_APIS_INCLUDE}/nvidia_utils/vulkan)

add_library(${VULKAN_VIDEO_DECODER_LIB} SHARED ${LIBVKVIDEODECODER_SRC})
# Link the libraries
target_link_libraries(${VULKAN_VIDEO_DECODER_LIB} PUBLIC ${libraries})
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_DECODER_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB} ${SHADERC_LIB})
target_compile_definitions(${VULKAN_VIDEO_DECODER_LIB} ${definitions})

target_include_directories(${VULKAN_VIDEO_DECODER_LIB} PUBLIC ${includes} ${VULKAN_VIDEO_DECODER_INCLUDE} ${VULKAN_VIDEO_DECODER_INCLUDE}/../NvVideoParser PRIVATE include)
target_compile_definitions(${VULKAN_VIDEO_DECODER_LIB}
    PRIVATE VK_VIDEO_DECODER_IMPLEMENTATION
    PUBLIC VK_VIDEO_DECODER_SHAREDLIB
)

find_package(Threads)

set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES SOVERSION ${VULKAN_VIDEO_DECODER_LIB_VERSION})

if(WIN32)
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES COMPILE_DEFINITIONS_DEBUG "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    set_target_properties(${VULKAN_VIDEO_DECODER_LIB} PROPERTIES LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS")
endif()

add_library(${VULKAN_VIDEO_DECODER_STATIC_LIB} STATIC ${LIBVKVIDEODECODER_SRC})
# Link the libraries
target_link_libraries(${VULKAN_VIDEO_DECODER_STATIC_LIB} PUBLIC ${libraries})
# Ensure the library depends on the generation of these files
add_dependencies(${VULKAN_VIDEO_DECODER_STATIC_LIB} GenerateDispatchTables ${VULKAN_VIDEO_PARSER_LIB} ${SHADERC_LIB})
target_include_directories(${VULKAN_VIDEO_DECODER_STATIC_LIB} PUBLIC ${includes} ${VULKAN_VIDEO_DECODER_INCLUDE} ${VULKAN_VIDEO_DECODER_INCLUDE}/../NvVideoParser PRIVATE include)
target_compile_definitions(${VULKAN_VIDEO_DECODER_STATIC_LIB} ${definitions})

install(TARGETS ${VULKAN_VIDEO_DECODER_LIB} ${VULKAN_VIDEO_DECODER_STATIC_LIB}
                RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

if(WIN32)
    install(TARGETS ${VULKAN_VIDEO_DECODER_LIB} ${VULKAN_VIDEO_DECODER_STATIC_LIB}
                    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
                    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
                    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
            )
endif()
