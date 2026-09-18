# Run a test binary and decide pass / fail / skip on the exit code AND the
# validation-layer output together.
#
# WHY A WRAPPER RATHER THAN set_tests_properties(FAIL_REGULAR_EXPRESSION):
# SKIP_RETURN_CODE outranks FAIL_REGULAR_EXPRESSION in CTest, so a run that
# emitted validation errors on its way to a 77 exit was recorded as Skipped.
# main.cpp has two 77 returns reached AFTER device creation, i.e. after the
# layer can already have spoken. A property-only gate cannot see both signals.
#
# ------------------------------------------------------------------------
# THE ECHO IS SANITISED, AND THAT IS LOAD-BEARING, NOT TIDINESS.
# ------------------------------------------------------------------------
# CTest decides SKIP by regex-matching the WHOLE captured output, and this
# script replays the child's stdout+stderr into that same space. An earlier
# version claimed the skip token "does not exist on any failing path" because
# only this script emits it. That guarantee was void: the child's text is in the
# match space too. Demonstrated end-to-end -- a directory named
# .../VALIDATION_GATE_RESULT=SKIP passed to VK_LAYER_PATH is echoed verbatim by
# main.cpp's provenance line, and a real run with 46 genuine validation errors
# was recorded "100% tests passed ... ***Skipped".
#
# So every occurrence of the token is defanged in the child's text before it is
# printed. The token then appears in the output only when THIS script writes it,
# on a path that has already established there were no validation errors.
#
# ------------------------------------------------------------------------
# WHAT IS COUNTED: MESSAGES, NOT VUID TOKENS.
# ------------------------------------------------------------------------
# The same defect yields a different token count depending on which reporter is
# active, with the library untouched:
#   * arms where the harness installs its own debug-utils messenger print a
#     "Validation Error: [ VUID-x ]" header AND a "The Vulkan spec states:
#     ...(VUID-x)" trailer -- TWO tokens per message, on stdout;
#   * --own-validate has no harness callback (the library owns the instance), so
#     the layer's default reporter prints ONE token per message, on stderr.
# Counting tokens made five arms look like "92 occurrences" against
# --own-validate's 46 when all five in fact have exactly 46 errors. A count that
# doubles under a reporter swap cannot distinguish "removed a defect" from
# "changed a message format". What both reporters emit exactly once per message
# is the "The Vulkan spec states:" trailer, so that closes a message block and
# the block carries the body the allowlist needs. Verified against both:
# default reporter tokens=46 headers=0 -> messages=46; messenger reporter
# tokens=92 headers=46 -> messages=46.
#
# CONTRACT
#   -DTEST_EXE=<path>       required
#   -DTEST_ARGS=<string>    optional, ONE argument or a space-separated string
#   -DREQUIRE_LAYER=<bool>  optional. ON: "no layer" is a failure, not a skip,
#                           so a fleet cannot sit green purely because the layer
#                           was missing everywhere.
#   -DALLOW_VUIDS=<regex>   optional, matched against the MESSAGE BODY.
#   -DRUN_TIMEOUT=<secs>    optional, default 600.

if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "validation_gate: TEST_EXE is required")
endif()
if(NOT DEFINED RUN_TIMEOUT OR RUN_TIMEOUT STREQUAL "")
  set(RUN_TIMEOUT 600)
endif()
if(NOT DEFINED ALLOW_VUIDS OR ALLOW_VUIDS STREQUAL "")
  # Matched against the BODY, deliberately, because the VUID NAME cannot carry
  # this distinction. VUID-Vk<Any>-pNext-pNext fires for two unrelated things:
  # a struct type the layer does not recognise (version skew, benign), and a
  # struct the layer knows perfectly well but which is NOT PERMITTED in that
  # chain -- a genuine defect, and exactly the kind a video-encode library that
  # chains many extension structs is at risk of. Only the body separates them.
  # An earlier version allowlisted on the name and would have passed a run whose
  # only message was "...which is not allowed here".
  #
  # SCOPE, measured, because it is easy to overstate: with the Chrome-bundled
  # layer there are ZERO skew messages on all seven arms. With the Vulkan SDK
  # layer they appear on six, but are the SOLE cause of redness on exactly ONE
  # (--context-conflict); the others are red from the genuine defect anyway. So
  # this allowlist keeps one arm honest, not six. "Older layer" is not the
  # explanation either -- both manifests declare api_version 1.4.304, and it is
  # the SDK build that reports skew while the newer Chrome-bundled one does not.
  set(ALLOW_VUIDS "unknown VkStructureType")
endif()

set(_args "")
if(DEFINED TEST_ARGS AND NOT TEST_ARGS STREQUAL "")
  separate_arguments(_args NATIVE_COMMAND "${TEST_ARGS}")
endif()

# TIMEOUT rather than streaming. execute_process buffers, so a hang would
# otherwise reach CTest's own TIMEOUT and take every captured byte with it --
# measured: "<end of output>", zero diagnostics, on a suite whose most plausible
# hang is a GPU encode. Timing out INSIDE the script keeps what was captured.
# Streaming (ECHO_*_VARIABLE) is not the answer: it would put the child's raw
# text back into CTest's match space and re-open the token collision above.
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

# Message count. Prefer the header, which exists exactly once per message when a
# debug-utils messenger is installed; fall back to raw tokens for the default
# reporter, which emits one per message.
string(REGEX MATCHALL "Validation Error" _headers "${_all}")
list(LENGTH _headers _n_headers)
string(REGEX MATCHALL "VUID-[A-Za-z0-9_]+-[A-Za-z0-9_-]+" _tokens "${_all}")
list(LENGTH _tokens _n_tokens)

# BLOCK-BASED, and the reason matters -- a line-based count is wrong for one of
# the two reporters and an earlier attempt silently passed a 46-error run
# because of it.
#
#   messenger reporter (arms where the harness installs its own callback):
#       "Validation Error: [ VUID-x ] ... <body>"      <- token here
#       "The Vulkan spec states: ... (...#VUID-x)"     <- token here too
#   default reporter (--own-validate; the library owns the instance so there is
#   no harness callback):
#       "vkQueueSubmit2KHR(): ... <body>"              <- NO token
#       "The Vulkan spec states: ... (...#VUID-x)"     <- token ONLY here
#
# So "skip the trailer" discards every message on the default reporter, and
# "count every token" double-counts on the messenger one. What both emit exactly
# once per message is the TRAILER, so the trailer closes a block and the block
# carries the body the allowlist needs.
string(REPLACE ";" "\\;" _lines "${_all}")
string(REPLACE "\n" ";" _lines "${_lines}")
set(_real 0)
set(_allowed 0)
set(_names "")
set(_block "")
foreach(_line IN LISTS _lines)
  set(_block "${_block}\n${_line}")
  if(_line MATCHES "The Vulkan spec states")
    if(_block MATCHES "VUID-[A-Za-z0-9_]+-[A-Za-z0-9_-]+")
      set(_vuid "${CMAKE_MATCH_0}")
      if(_block MATCHES "${ALLOW_VUIDS}")
        math(EXPR _allowed "${_allowed}+1")
      else()
        math(EXPR _real "${_real}+1")
        list(APPEND _names "${_vuid}")
      endif()
    endif()
    set(_block "")
  endif()
endforeach()
# A layer that emits a VUID with no trailer at all would leave a dangling block;
# count it rather than lose it.
if(_block MATCHES "VUID-[A-Za-z0-9_]+-[A-Za-z0-9_-]+")
  set(_vuid "${CMAKE_MATCH_0}")
  if(_block MATCHES "${ALLOW_VUIDS}")
    math(EXPR _allowed "${_allowed}+1")
  else()
    math(EXPR _real "${_real}+1")
    list(APPEND _names "${_vuid}")
  endif()
endif()
if(_names)
  list(REMOVE_DUPLICATES _names)
  string(REPLACE ";" " " _names "${_names}")
endif()

message("VALIDATION_GATE: exit=${_rc} messages=${_real} allowlisted=${_allowed} "
        "(raw tokens=${_n_tokens}, headers=${_n_headers})")

# Allowlisted messages are tolerated but never silent. On a green run CTest
# shows no output at all, so an accumulation of them would otherwise be
# invisible forever; this line is appended to a file that survives the run.
if(DEFINED GATE_SUMMARY AND NOT GATE_SUMMARY STREQUAL "")
  file(APPEND "${GATE_SUMMARY}"
       "${TEST_EXE} ${TEST_ARGS}: exit=${_rc} messages=${_real} allowlisted=${_allowed}\n")
endif()

# ------------------------------------------------------------------------
# THE ENCODE-SOURCE LAYOUT REGRESSION, WHICH NO VALIDATION MESSAGE CAN CARRY.
# ------------------------------------------------------------------------
# VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811 is checked against the image-layout
# map of the command buffer the encode is recorded into. The staged input's
# barriers are recorded into a DIFFERENT command buffer, so that map has no
# entry for the encode-source image and the check returns true without
# comparing anything -- and the submit-time sweep reads the same registry, so it
# is blind in the same way. CF-02a and CF-02b lived in that blind spot through
# a run reported as "144 -> 0 validation messages"; zero was the correct count
# for a check that never ran.
#
# So the counter above CANNOT see this defect class, and a gate built only on it
# is a gate that passes an encode reading its source in TRANSFER_DST_OPTIMAL.
# VkVideoEncoder::RecordVideoCodingCmd emits its own unconditional diagnostic
# when the staging arm left the image in anything other than
# VIDEO_ENCODE_SRC_KHR; this promotes that from loud to gating.
#
# DEMONSTRATED IN BOTH DIRECTIONS, because a gate that has never been red is not
# known to be a gate: with the hand-off barriers present the two staged arms
# emit zero of these, and with them deleted the copy arm emits 8 and the filter
# arm 60.
string(REGEX MATCHALL "staged encode-source image is in layout" _layout_hits "${_all}")
list(LENGTH _layout_hits _n_layout)
if(_n_layout GREATER 0)
  message(FATAL_ERROR
          "validation_gate: ${_n_layout} frame(s) reached vkCmdEncodeVideoKHR"
          " with the staged encode-source image in the wrong layout"
          " (VUID-vkCmdEncodeVideoKHR-pEncodeInfo-10811). A StageInputFrame arm"
          " is missing its hand-off barrier to VIDEO_ENCODE_SRC_KHR. This is"
          " invisible to the validation layer -- see the note above -- so this"
          " check is the only thing that reports it.")
endif()

if(_real GREATER 0)
  message(FATAL_ERROR
          "validation_gate: ${_real} validation message(s): ${_names}"
          " (exit=${_rc}, ${_allowed} allowlisted by body /${ALLOW_VUIDS}/)")
endif()

if(_rc EQUAL 77)
  if(REQUIRE_LAYER)
    message(FATAL_ERROR
            "validation_gate: the run skipped (77) but REQUIRE_LAYER is ON."
            " A skipped validation arm proves nothing; set"
            " VVS_VALIDATION_LAYER_PATH, or configure with"
            " -DVVS_REQUIRE_VALIDATION_LAYER=OFF to allow skipping.")
  endif()
  message("VALIDATION_GATE_RESULT=SKIP")
  return()
endif()

# _rc is a STRING on abnormal exit ("Segmentation fault", "Process terminated
# due to timeout", "no such file or directory"). EQUAL comparisons are correctly
# false for those, so both branches above fall through to here and fail.
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "validation_gate: test did not exit cleanly: ${_rc}")
endif()
