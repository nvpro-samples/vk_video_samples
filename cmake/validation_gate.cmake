# Run a test binary and decide pass / fail / skip on the exit code AND the
# validation-layer output together.
#
# WHY A WRAPPER RATHER THAN set_tests_properties(FAIL_REGULAR_EXPRESSION):
# SKIP_RETURN_CODE outranks FAIL_REGULAR_EXPRESSION in CTest, so a run that
# emits validation errors on its way to a skip exit is recorded as Skipped and
# the errors are lost. A property-only gate cannot see both signals.
#
# THE ECHO IS SANITISED. CTest decides SKIP by regex-matching the whole
# captured output, and this script replays the child stdout and stderr into
# that same space. Every occurrence of the skip token in the child text is
# defanged before it is printed, so the token appears only where THIS script
# writes it, on a path that has already established there were no unexpected
# validation messages.
#
# WHAT IS COUNTED: MESSAGES, NOT VUID TOKENS. A binary that installs its own
# debug-utils messenger prints a "Validation Error: [ VUID-x ]" header AND a
# "The Vulkan spec states: ...(VUID-x)" trailer, two tokens per message; the
# layer default reporter prints the trailer only. Counting tokens therefore
# doubles under a reporter swap, which cannot distinguish a removed defect
# from a changed message format. What both reporters emit exactly once per
# message is the trailer, so the trailer closes a block and the block carries
# the body and the VUID name.
#
# THE LAYER IS FORCED, AND ITS INSERTION IS WITNESSED. A gated test that runs
# without the validation layer loaded is a check that cannot fail, and it
# looks exactly like a clean run. The layer is forced on through the loader
# rather than left to the binary, so a binary that enables validation only
# under a verbose flag is gated too; and the loader is asked to name the
# layers it inserts, so "no messages" is told apart from "no layer".
#
# WHAT A MISSING LAYER COSTS, AND WHAT IT DOES NOT. It costs the validation
# verdict, and only that: the test still runs and its own exit code still
# decides pass or fail. Reporting a skip instead would make this script
# STRICTLY WEAKER than the plain add_test() it replaces on every host with no
# installed layer -- a check that cannot fail, which is the failure this
# script exists to prevent, turned on the script itself. REQUIRE_LAYER is how
# a fleet makes the missing layer a failure in its own right.
#
# CONTRACT
#   -DTEST_EXE=<path>        required
#   -DTEST_ARGS=<string>     optional, one argument or a space-separated string
#   -DREQUIRE_LAYER=<bool>   optional. ON: a missing layer is a failure rather
#                            than an ungated run, so a fleet cannot sit green
#                            purely because the layer was absent everywhere.
#                            OFF, the default, still runs the test and still
#                            reports its exit code -- a missing layer removes
#                            the validation verdict and nothing else.
#   -DALLOW_VUIDS=<regex>    optional, matched against the message BODY.
#   -DEXPECT_VUIDS=<list>    optional, comma-separated <VUID>=<count> ceilings.
#   -DGATE_SUMMARY=<path>    optional, a file every run appends its tally to.
#   -DRUN_TIMEOUT=<secs>     optional, default 600.

if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "validation_gate: TEST_EXE is required")
endif()
if(NOT DEFINED RUN_TIMEOUT OR RUN_TIMEOUT STREQUAL "")
  set(RUN_TIMEOUT 600)
endif()
if(NOT DEFINED ALLOW_VUIDS OR ALLOW_VUIDS STREQUAL "")
  # Matched against the BODY, deliberately, because the VUID NAME cannot carry
  # this distinction. VUID-Vk<Any>-pNext-pNext fires both for a struct type the
  # layer does not recognise -- header and layer built against different
  # versions, benign -- and for a struct the layer knows perfectly well but
  # which is not permitted in that chain, which is a real defect and one a
  # video-encode library that chains many extension structures is exposed to.
  # Only the body separates them.
  set(ALLOW_VUIDS "unknown VkStructureType")
endif()

set(_args "")
if(DEFINED TEST_ARGS AND NOT TEST_ARGS STREQUAL "")
  # Accept both spellings. A caller that forwards a CMake list hands over
  # "--arm;--validate", which carries no whitespace for separate_arguments to
  # split on and would reach the binary as one unrecognised argument.
  string(REPLACE ";" " " _test_args_norm "${TEST_ARGS}")
  separate_arguments(_args NATIVE_COMMAND "${_test_args_norm}")
endif()

# The declared ceilings, one CMake variable per VUID.
set(_expected_names "")
if(DEFINED EXPECT_VUIDS AND NOT EXPECT_VUIDS STREQUAL "")
  string(REPLACE "," ";" _expect_list "${EXPECT_VUIDS}")
  foreach(_entry IN LISTS _expect_list)
    string(STRIP "${_entry}" _entry)
    if(NOT _entry STREQUAL "")
      if(NOT _entry MATCHES "^(VUID-[A-Za-z0-9_]+-[A-Za-z0-9_-]+)=([0-9]+)$")
        message(FATAL_ERROR
                "validation_gate: EXPECT_VUIDS entry is not <VUID>=<count>: ${_entry}")
      endif()
      set(_exp_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
      list(APPEND _expected_names "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()

# TIMEOUT rather than streaming. execute_process buffers, so a hang would
# otherwise reach the CTest timeout and take every captured byte with it.
# Streaming would put the child raw text back into the CTest match space and
# re-open the skip-token collision described above.
execute_process(
  COMMAND "${TEST_EXE}" ${_args}
  TIMEOUT ${RUN_TIMEOUT}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE  _err)

set(_all "${_out}${_err}")
set(_safe "${_all}")
string(REPLACE "VALIDATION_GATE_RESULT" "VALIDATION_GATE_RESULT_FROM_CHILD" _safe "${_safe}")
message("${_safe}")

# The layer witness. The loader names each layer it inserts when it is asked
# to, so this line is present on every run the layer joined and absent on
# every run it did not, which is what separates a clean run from a blind one.
string(REGEX MATCHALL "Insert[a-z]* instance layer .VK_LAYER_KHRONOS_validation."
       _layer_witness "${_all}")
list(LENGTH _layer_witness _n_witness)
set(_have_layer TRUE)
if(_n_witness EQUAL 0)
  if(REQUIRE_LAYER)
    message(FATAL_ERROR
            "validation_gate: the validation layer was never inserted, so this"
            " run gated nothing. Point VVS_VALIDATION_LAYER_PATH (and"
            " VVS_VALIDATION_LAYER_LIBDIR) at an installed layer, or configure"
            " with -DVVS_REQUIRE_VALIDATION_LAYER=OFF to allow an ungated"
            " run.")
  endif()
  # A MISSING LAYER IS NOT A SKIP. The child has already run, above, and its
  # exit code is the verdict it was written to give -- the same verdict it
  # gave before it was gated. Returning here would discard that and report
  # Skipped, which would make gating a test STRICTLY WEAKER than leaving it on
  # a plain add_test(): on every runner without an installed layer, and that
  # is the default runner, a gated test would report Skipped whether it passed
  # or failed. So this arm records what was not checked and falls through to
  # the exit-code check at the end. The only skip this script emits is the
  # child's own exit 77.
  set(_have_layer FALSE)
  message("VALIDATION_GATE: no validation layer was inserted; nothing was"
          " gated, and this run stands on the test's own exit code")
endif()

# Block-based counting. A line-based count is wrong for one of the two
# reporters: the default reporter carries the VUID token on the trailer only,
# so skipping trailers discards every message it emits, while counting every
# token double-counts the messenger reporter. The trailer closes the block,
# and the block carries the body the allowlist is matched against.
string(REPLACE ";" "\\;" _lines "${_all}")
string(REPLACE "\n" ";" _lines "${_lines}")
set(_real 0)
set(_allowed 0)
set(_seen_names "")
set(_block "")

macro(_vvs_tally _text)
  if("${_text}" MATCHES "VUID-[A-Za-z0-9_]+-[A-Za-z0-9_-]+")
    set(_vuid "${CMAKE_MATCH_0}")
    if("${_text}" MATCHES "${ALLOW_VUIDS}")
      math(EXPR _allowed "${_allowed}+1")
    else()
      math(EXPR _real "${_real}+1")
      if(NOT DEFINED _seen_${_vuid})
        set(_seen_${_vuid} 0)
        list(APPEND _seen_names "${_vuid}")
      endif()
      math(EXPR _seen_${_vuid} "${_seen_${_vuid}}+1")
    endif()
  endif()
endmacro()

foreach(_line IN LISTS _lines)
  set(_block "${_block}\n${_line}")
  if(_line MATCHES "The Vulkan spec states")
    _vvs_tally("${_block}")
    set(_block "")
  endif()
endforeach()
# A layer that emits a VUID with no trailer at all leaves a dangling block;
# count it rather than lose it.
_vvs_tally("${_block}")

# Sort the tally into what the ceilings cover and what they do not. A VUID
# with no ceiling fails on its first message, and a VUID with one fails on the
# first message past it, so neither a new VUID nor a growing one can hide
# behind an allowance.
set(_over "")
set(_tolerated "")
set(_under "")
foreach(_vuid IN LISTS _seen_names)
  set(_n "${_seen_${_vuid}}")
  set(_limit 0)
  if(DEFINED _exp_${_vuid})
    set(_limit "${_exp_${_vuid}}")
  endif()
  if(_n GREATER _limit)
    list(APPEND _over "${_vuid} ${_n}/${_limit}")
  else()
    list(APPEND _tolerated "${_vuid}=${_n}/${_limit}")
  endif()
endforeach()
foreach(_vuid IN LISTS _expected_names)
  if(NOT DEFINED _seen_${_vuid})
    list(APPEND _under "${_vuid}=0/${_exp_${_vuid}}")
  endif()
endforeach()

string(REPLACE ";" " " _tolerated_txt "${_tolerated}")
string(REPLACE ";" ", " _over_txt "${_over}")
string(REPLACE ";" " " _under_txt "${_under}")

# A tally taken with no layer is zero for the reason zero always looks like on
# this instrument -- nothing was watching -- so it is reported as unmeasured
# rather than as clean, here and in the summary file.
string(CONCAT _gate_tally "messages=${_real} skew-allowlisted=${_allowed}"
                           " tolerated=[${_tolerated_txt}]")
if(NOT _have_layer)
  set(_gate_tally "messages=NOT-MEASURED (no validation layer was inserted)")
endif()
message("VALIDATION_GATE: exit=${_rc} ${_gate_tally}")
if(_under AND _have_layer)
  # An enumerated ceiling that is no longer reached is stale. It is reported
  # rather than enforced downwards, because a count that moves with the driver
  # would otherwise turn the gate red for an improvement.
  message("VALIDATION_GATE: ceiling no longer reached, lower or remove it:"
          " ${_under_txt}")
endif()

# Skew-allowlisted and tolerated messages are never silent. On a green run
# CTest shows no output at all, so an accumulation of them would otherwise be
# invisible; this line survives the run in a file.
if(DEFINED GATE_SUMMARY AND NOT GATE_SUMMARY STREQUAL "")
  file(APPEND "${GATE_SUMMARY}"
       "${TEST_EXE} ${TEST_ARGS}: exit=${_rc} ${_gate_tally}\n")
endif()

# THE ENCODE-SOURCE LAYOUT, WHICH NO VALIDATION MESSAGE CARRIES.
# VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 is checked against the image
# layout map of the command buffer the encode is recorded into. A staged input
# has its barriers recorded into a DIFFERENT command buffer, so that map holds
# no entry for the encode-source image and the check returns true without
# comparing anything; the submit-time sweep reads the same registry and is
# blind in the same way. The encoder emits its own diagnostic when the staging
# arm leaves the image in anything other than VIDEO_ENCODE_SRC_KHR, and this
# promotes that from loud to gating.
string(REGEX MATCHALL "staged encode-source image is in layout"
       _layout_hits "${_all}")
list(LENGTH _layout_hits _n_layout)
if(_n_layout GREATER 0)
  message(FATAL_ERROR
          "validation_gate: ${_n_layout} frame(s) reached vkCmdEncodeVideoKHR"
          " with the staged encode-source image in the wrong layout"
          " (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811). A staging arm is"
          " missing its hand-off barrier to VIDEO_ENCODE_SRC_KHR. The"
          " validation layer cannot see this, so this check is the only thing"
          " that reports it.")
endif()

if(_over)
  message(FATAL_ERROR
          "validation_gate: validation message(s) past their declared ceiling:"
          " ${_over_txt}. A VUID with no declared ceiling has a ceiling of zero."
          " (exit=${_rc}, ${_allowed} skew-allowlisted by body /${ALLOW_VUIDS}/)")
endif()

if(_rc EQUAL 77)
  message("VALIDATION_GATE_RESULT=SKIP")
  return()
endif()

# _rc is a STRING on abnormal exit ("Segmentation fault", "Process terminated
# due to timeout", "no such file or directory"). EQUAL comparisons are
# correctly false for those, so they fall through to here and fail.
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "validation_gate: test did not exit cleanly: ${_rc}")
endif()
