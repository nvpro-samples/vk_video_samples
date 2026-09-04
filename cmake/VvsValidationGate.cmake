# Register a CTest entry whose validation-layer output is gating.
#
# THE NAME IS DELIBERATE. Two encoder test directories carry their own gate
# script and define a function of their own, vvs_add_gated_test(). A CMake
# function is global from the point it is defined, so a shared definition
# sharing that name would be replaced by whichever directory the configure
# reached last and every directory after it would silently get the other
# implementation. The two names are distinct so that neither can shadow the
# other.
#
# A test added with add_test() alone reports only its exit code, so a run that
# emitted validation errors and still exited 0 is recorded as PASSED and the
# errors are visible to nobody. vvs_add_validation_gated_test() runs the same
# binary through cmake/validation_gate.cmake, which reads the exit code and
# the layer output together and fails the test that produced a message.
#
#   vvs_add_validation_gated_test(<test-name>
#                      TARGET <executable-target>
#                      [ARGS <arg>...]
#                      [EXPECT <VUID>=<count>...])
#
# EXPECT declares a CEILING per VUID, and it is the only way a message is
# tolerated. A VUID that is not named has a ceiling of zero, so a message the
# tree has never seen fails on its first occurrence; a VUID that is named
# fails on the first occurrence past its count. Both facts are printed on
# every run and appended to validation_gate_summary.txt in the build root, so
# an allowance is never silent. An EXPECT entry records a defect that the tree
# already carries and that the entry does not excuse -- name the defect where
# the entry is written.
#
# WHAT PINS WHAT. VK_LAYER_SETTINGS_PATH controls HOW MANY messages print;
# the layer path controls WHETHER ANY DO. VVS_VALIDATION_LAYER_PATH is empty
# by default, and while it is empty the ambient environment decides which
# layer the loader finds -- so set it to make a build self-contained, and set
# VVS_REQUIRE_VALIDATION_LAYER so that a host without a layer reports a
# failure rather than an ungated run.
#
# A GATED TEST STILL RUNS WHERE NO LAYER IS PRESENT, and its own exit code
# still decides its verdict; what a missing layer removes is the validation
# check and nothing else. That is what makes gating a test never weaker than
# leaving it on a plain add_test(): this adds a way to fail and takes none
# away.

set(VVS_VALIDATION_ALLOW_VUIDS "unknown VkStructureType" CACHE STRING
    "Regex matched against a validation message BODY; matches are tolerated")
option(VVS_REQUIRE_VALIDATION_LAYER
       "Fail, rather than skip, a gated test when no validation layer is installed"
       OFF)
set(VVS_VALIDATION_LAYER_PATH "" CACHE PATH
    "Directory holding a validation-layer manifest; becomes the tests VK_LAYER_PATH")
set(VVS_VALIDATION_LAYER_LIBDIR "" CACHE PATH
    "Directory added to the tests LD_LIBRARY_PATH. A Vulkan SDK layer manifest names a bare soname, so without this the layer loads only if its library is already on the default search path")

set(VVS_VALIDATION_GATE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/validation_gate.cmake")
set(VVS_VALIDATION_LAYER_SETTINGS "${CMAKE_CURRENT_LIST_DIR}/vk_layer_settings.txt")

function(vvs_add_validation_gated_test _name)
  cmake_parse_arguments(_g "" "TARGET" "ARGS;EXPECT" ${ARGN})
  if(NOT _g_TARGET)
    message(FATAL_ERROR "vvs_add_validation_gated_test(${_name}): TARGET is required")
  endif()

  # SPACE-separated. validation_gate.cmake documents TEST_ARGS as one argument
  # or a space-separated string and splits it on whitespace; a CMake list
  # interpolates semicolon-separated, which would reach the binary as a single
  # unrecognised argument and make it print its usage and exit non-zero.
  string(JOIN " " _gated_args ${_g_ARGS})
  # Comma-separated for the same reason, and VUID names carry no commas.
  string(JOIN "," _gated_expect ${_g_EXPECT})

  set(_env
      "VK_LAYER_SETTINGS_PATH=${VVS_VALIDATION_LAYER_SETTINGS}"
      # Forced through the loader rather than left to the binary: a binary
      # that enables the layer only under a verbose flag would otherwise be
      # gated by a layer that never loaded.
      "VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation"
      # Makes the loader name the layers it inserts. The gate requires that
      # line before it reads a count, so a run with no layer is reported as
      # such instead of as a clean one.
      "VK_LOADER_DEBUG=layer")
  if(VVS_VALIDATION_LAYER_PATH)
    list(APPEND _env "VK_LAYER_PATH=${VVS_VALIDATION_LAYER_PATH}")
  endif()
  if(VVS_VALIDATION_LAYER_LIBDIR)
    # Replaces rather than prepends. Deliberate, and opt-in: a test whose
    # loader path half-comes from the ambient shell is not reproducible.
    list(APPEND _env "LD_LIBRARY_PATH=${VVS_VALIDATION_LAYER_LIBDIR}")
  endif()

  add_test(NAME ${_name}
           COMMAND ${CMAKE_COMMAND}
                   -DTEST_EXE=$<TARGET_FILE:${_g_TARGET}>
                   "-DTEST_ARGS=${_gated_args}"
                   -DREQUIRE_LAYER=${VVS_REQUIRE_VALIDATION_LAYER}
                   "-DALLOW_VUIDS=${VVS_VALIDATION_ALLOW_VUIDS}"
                   "-DEXPECT_VUIDS=${_gated_expect}"
                   "-DGATE_SUMMARY=${CMAKE_BINARY_DIR}/validation_gate_summary.txt"
                   -P "${VVS_VALIDATION_GATE_SCRIPT}")
  # SKIP_REGULAR_EXPRESSION, not SKIP_RETURN_CODE: SKIP_RETURN_CODE outranks
  # every other verdict in CTest, so a run that emitted validation errors on
  # its way to a skip exit would be recorded as Skipped. The gate emits the
  # token below only on a path that has already established there were none,
  # and only for the test's own exit 77 -- the same condition SKIP_RETURN_CODE
  # 77 named on these tests before they were gated. SKIP_RETURN_CODE cannot be
  # carried alongside it in any case: the registered command is the cmake -P
  # wrapper, whose exit code is 0 or 1 and never 77.
  set_tests_properties(${_name} PROPERTIES
                       SKIP_REGULAR_EXPRESSION "VALIDATION_GATE_RESULT=SKIP"
                       ENVIRONMENT "${_env}")
endfunction()
