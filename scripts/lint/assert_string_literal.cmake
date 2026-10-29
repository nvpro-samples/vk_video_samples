# Fails when a C or C++ source of the decoder, the encoder or the common code asserts on a string
# literal. assert("...") is always true and never fires: an internal invariant is assert(!"..."),
# and an error in a bitstream is logged and refused, not asserted.
#
#   cmake -DROOT=<source tree> -P assert_string_literal.cmake

file(GLOB_RECURSE sources LIST_DIRECTORIES false
     "${ROOT}/vk_video_decoder/*.c" "${ROOT}/vk_video_decoder/*.cpp" "${ROOT}/vk_video_decoder/*.h" "${ROOT}/vk_video_decoder/*.hpp"
     "${ROOT}/vk_video_encoder/*.c" "${ROOT}/vk_video_encoder/*.cpp" "${ROOT}/vk_video_encoder/*.h" "${ROOT}/vk_video_encoder/*.hpp"
     "${ROOT}/common/*.c" "${ROOT}/common/*.cpp" "${ROOT}/common/*.h" "${ROOT}/common/*.hpp")
if(NOT sources)
    message(FATAL_ERROR "No sources under ${ROOT}")
endif()
set(found "")
foreach(source ${sources})
    file(STRINGS "${source}" lines REGEX "assert[ \t]*\\([ \t]*\"")
    foreach(line ${lines})
        string(STRIP "${line}" line)
        string(APPEND found "  ${source}: ${line}\n")
    endforeach()
endforeach()
if(found)
    message(FATAL_ERROR "assert() on a string literal, which never fires:\n${found}")
endif()
list(LENGTH sources count)
message(STATUS "${count} sources, no assert() on a string literal")
