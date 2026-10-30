# vvs_report_fetched(<name>): prints the commit FetchContent checked out for <name>, so that a build
# log names exactly the code it built against. With FETCHCONTENT_FULLY_DISCONNECTED the checkout is
# whatever the build tree already holds, which the declared GIT_TAG does not tell.
include_guard(GLOBAL)

function(vvs_report_fetched _name)
    string(TOLOWER "${_name}" _lower)
    set(_dir "${${_lower}_SOURCE_DIR}")
    find_package(Git QUIET)
    set(_commit "")
    if(GIT_FOUND AND EXISTS "${_dir}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_dir}" rev-parse HEAD
                        OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(_commit STREQUAL "")
        message(STATUS "Fetched ${_name}: ${_dir}, commit unknown (no git checkout)")
    else()
        message(STATUS "Fetched ${_name}: ${_commit}")
    endif()
endfunction()
