# Source lints that need no build and no device.
add_test(NAME LintAssertStringLiteral
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_string_literal.cmake)
set_tests_properties(LintAssertStringLiteral PROPERTIES LABELS "device-free")
add_test(NAME LintGitTagPinned
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/git_tag_pinned.cmake)
set_tests_properties(LintGitTagPinned PROPERTIES LABELS "device-free")
# The pattern LintGitTagPinned applies: release tags and a 40-digit commit pass; branches, a
# 39-digit and a 41-digit hex string do not.
add_test(NAME LintGitTagPinnedPattern
         COMMAND ${CMAKE_COMMAND}
                 -DACCEPT=0123456789abcdef0123456789abcdef01234567,v1.4.365,16.6.0,vulkan-sdk-1.4.363.0,v2026.4
                 -DREFUSE=main,master,vulkan-sdk-main,0123456789abcdef0123456789abcdef0123456,0123456789abcdef0123456789abcdef012345678
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/git_tag_pinned.cmake)
set_tests_properties(LintGitTagPinnedPattern PROPERTIES LABELS "device-free")
