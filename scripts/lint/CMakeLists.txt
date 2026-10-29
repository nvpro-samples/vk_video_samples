# Source lints that need no build and no device.
add_test(NAME LintAssertStringLiteral
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_string_literal.cmake)
set_tests_properties(LintAssertStringLiteral PROPERTIES LABELS "device-free")
