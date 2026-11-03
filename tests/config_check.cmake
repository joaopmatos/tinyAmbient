# Config persistence, status output, and portable mode.
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -DVERSION=<v> -P tests/config_check.cmake
#
# Every run gets a sandboxed XDG_CONFIG_HOME (Linux) and APPDATA (Windows), so
# the test never touches the developer's real configuration. A sandboxed
# XDG_RUNTIME_DIR (Linux) or a private pipe name (Windows) keeps the daemon that
# content commands start from colliding with a real session; each such daemon is
# quit before the next section touches the file.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR OR NOT DEFINED VERSION)
    message(FATAL_ERROR "config_check: BINARY, WORKDIR and VERSION are required")
endif()

set(SANDBOX "${WORKDIR}/config_check")
file(REMOVE_RECURSE "${SANDBOX}")
file(MAKE_DIRECTORY "${SANDBOX}")

set(XDG "${SANDBOX}/xdg")
set(APPDATA "${SANDBOX}/appdata")
set(RUNTIME "${SANDBOX}/runtime")
file(MAKE_DIRECTORY "${RUNTIME}")

if(WIN32)
    set(CFG "${APPDATA}/tinyAmbient/config.ini")
else()
    set(CFG "${XDG}/tinyambient/config.ini")
endif()
set(PLAYING 1)

# Windows has no per-test runtime directory, so the IPC pipe is named explicitly
# and the run cannot reach the developer's real daemon.
string(MD5 IPC_TAG "${WORKDIR}")
if(WIN32)
    set(IPC_NAME "\\\\.\\pipe\\tinyambient-config-${IPC_TAG}")
else()
    set(IPC_NAME "")
endif()

function(run_exe exe)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_CONFIG_HOME=${XDG}"
            "XDG_RUNTIME_DIR=${RUNTIME}"
            "APPDATA=${APPDATA}"
            "TINYAMBIENT_AUDIO=null"
            "TINYAMBIENT_IPC=${IPC_NAME}"
            "${exe}" ${ARGN}
        RESULT_VARIABLE result
        TIMEOUT 60
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)

    set(TA_OUT "${output}" PARENT_SCOPE)
    set(TA_ERR "${errors}" PARENT_SCOPE)
    set(TA_CODE "${result}" PARENT_SCOPE)
endfunction()

function(expect_ok label)
    if(NOT TA_CODE EQUAL 0)
        message(FATAL_ERROR
            "config_check: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_OUT STREQUAL "")
        message(FATAL_ERROR
            "config_check: ${label}: unexpected stdout: ${TA_OUT}")
    endif()
endfunction()

function(expect_status_ok label)
    if(NOT TA_CODE EQUAL 0)
        message(FATAL_ERROR
            "config_check: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_ERR STREQUAL "")
        message(FATAL_ERROR
            "config_check: ${label}: unexpected stderr: ${TA_ERR}")
    endif()
endfunction()

function(expect_status label playing noise volume mono)
    # VERSION comes from the project; a hardcoded copy breaks on every bump.
    set(expected
        "version=${VERSION}\nplaying=${playing}\nnoise=${noise}\nvolume=${volume}\nmono=${mono}\nstop_after=off\n")
    if(NOT TA_OUT STREQUAL expected)
        message(FATAL_ERROR
            "config_check: ${label}: status mismatch\nexpected:\n${expected}got:\n${TA_OUT}")
    endif()
endfunction()

# Stops the daemon a content command may have started. On POSIX the daemon is
# what persists the setting, so it has to be quit before the test edits the
# file behind its back.
function(quit_daemon)
    run_exe("${BINARY}" --quit)
    if(NOT TA_CODE EQUAL 0)
        message(FATAL_ERROR
            "config_check: quit failed with ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
endfunction()

# --- Status without a config file: defaults, and no write --------------------

run_exe("${BINARY}" --status)
expect_status_ok("status without a daemon")
expect_status("status defaults" 0 pink 20 0)
if(EXISTS "${CFG}")
    message(FATAL_ERROR "config_check: status created ${CFG}")
endif()

# --- Persisted settings ------------------------------------------------------
#
# On POSIX the first content command starts a daemon and the setting is
# persisted by the daemon, not by the client (PRD FR-06, section 7 rule 7).
# The status after it therefore reports playing=1.

run_exe("${BINARY}" --volume 25)
expect_ok("volume 25")
if(NOT EXISTS "${CFG}")
    message(FATAL_ERROR "config_check: --volume did not write ${CFG}")
endif()

run_exe("${BINARY}" --status)
expect_status_ok("status after volume")
expect_status("volume persisted" ${PLAYING} pink 25 0)

run_exe("${BINARY}" --noise brown --volume 60 --mono)
expect_ok("noise, volume, mono")

run_exe("${BINARY}" --status)
expect_status_ok("status after three settings")
expect_status("settings persisted" ${PLAYING} brown 60 1)

run_exe("${BINARY}" --stereo)
expect_ok("stereo")
run_exe("${BINARY}" --status)
expect_status_ok("status after stereo")
expect_status("stereo persisted" ${PLAYING} brown 60 0)

quit_daemon()

# --- Usage errors never destroy or rewrite the config ------------------------

file(READ "${CFG}" before_invalid)
run_exe("${BINARY}" --volume 101)
file(READ "${CFG}" after_invalid)
if(NOT TA_CODE EQUAL 2)
    message(FATAL_ERROR
        "config_check: volume 101: exit ${TA_CODE}, expected 2")
endif()
if(NOT TA_ERR MATCHES "^tinyambient: ")
    message(FATAL_ERROR
        "config_check: volume 101: stderr lacks the prefix: ${TA_ERR}")
endif()
if(NOT after_invalid STREQUAL before_invalid)
    message(FATAL_ERROR "config_check: a usage error rewrote the config")
endif()

# Session commands change no file. Without a daemon they start one, apply the
# timer, and leave the configuration untouched.
run_exe("${BINARY}" --stop-after 5)
file(READ "${CFG}" after_session)
if(NOT after_session STREQUAL before_invalid)
    message(FATAL_ERROR "config_check: stop-after rewrote the config")
endif()
quit_daemon()

# --- Status and render never write ------------------------------------------

file(READ "${CFG}" before_status)
run_exe("${BINARY}" --status)
expect_status_ok("status is read-only")
file(READ "${CFG}" after_status)
if(NOT after_status STREQUAL before_status)
    message(FATAL_ERROR "config_check: status rewrote the config")
endif()

file(READ "${CFG}" before_render)
run_exe("${BINARY}" --render "${SANDBOX}/render.raw" --seconds 1)
expect_ok("render")
file(READ "${CFG}" after_render)
if(NOT after_render STREQUAL before_render)
    message(FATAL_ERROR "config_check: render rewrote the config")
endif()

# --- Unknown keys and comments survive a rewrite -----------------------------

file(WRITE "${CFG}"
    "# top comment\n"
    "[general]\n"
    "noise=white\n"
    "# keep me\n"
    "future=42\n"
    "[other]\n"
    "; section comment\n"
    "x=1\n")

run_exe("${BINARY}" --volume 33)
expect_ok("rewrite with unknown keys")
quit_daemon()
file(READ "${CFG}" rewritten)
if(NOT rewritten MATCHES "future=42")
    message(FATAL_ERROR "config_check: unknown key was dropped:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "# top comment")
    message(FATAL_ERROR "config_check: comment was dropped:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "# keep me")
    message(FATAL_ERROR "config_check: comment was dropped:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "; section comment")
    message(FATAL_ERROR "config_check: comment was dropped:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "\\[other\\]")
    message(FATAL_ERROR "config_check: unknown section was dropped:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "volume=33")
    message(FATAL_ERROR "config_check: volume was not written:\n${rewritten}")
endif()
if(NOT rewritten MATCHES "noise=white")
    message(FATAL_ERROR "config_check: noise was not preserved:\n${rewritten}")
endif()

# --- Malformed values warn, fall back, and are not rewritten ----------------

file(WRITE "${CFG}"
    "[general]\n"
    "noise=gray\n"
    "volume=101\n")

file(READ "${CFG}" before_broken)
run_exe("${BINARY}" --status)
if(NOT TA_CODE EQUAL 0)
    message(FATAL_ERROR
        "config_check: malformed config aborted startup with ${TA_CODE}")
endif()
if(NOT TA_ERR MATCHES "^tinyambient: ")
    message(FATAL_ERROR
        "config_check: malformed config produced no warning: ${TA_ERR}")
endif()
expect_status("malformed values use defaults" 0 pink 20 0)
file(READ "${CFG}" after_broken)
if(NOT after_broken STREQUAL before_broken)
    message(FATAL_ERROR "config_check: status rewrote a malformed config")
endif()

# --- Portable mode -----------------------------------------------------------

get_filename_component(BINARY_NAME "${BINARY}" NAME)
set(PORTABLE_DIR "${SANDBOX}/portable")
file(MAKE_DIRECTORY "${PORTABLE_DIR}")
file(COPY "${BINARY}" DESTINATION "${PORTABLE_DIR}")
set(PORTABLE_BIN "${PORTABLE_DIR}/${BINARY_NAME}")

# Point XDG/APPDATA somewhere untouched so the test proves portable mode does
# not fall back to them.
set(XDG "${SANDBOX}/xdg_unused")
set(APPDATA "${SANDBOX}/appdata_unused")

run_exe("${PORTABLE_BIN}" --portable --volume 77)
expect_ok("portable volume")
if(NOT EXISTS "${PORTABLE_DIR}/config.ini")
    message(FATAL_ERROR
        "config_check: --portable did not create config.ini next to the binary")
endif()
if(EXISTS "${XDG}/tinyambient/config.ini")
    message(FATAL_ERROR
        "config_check: --portable wrote to XDG_CONFIG_HOME")
endif()
if(EXISTS "${APPDATA}/tinyAmbient/config.ini")
    message(FATAL_ERROR "config_check: --portable wrote to APPDATA")
endif()

run_exe("${PORTABLE_BIN}" --portable --status)
expect_status_ok("portable status")
expect_status("portable setting persisted" ${PLAYING} pink 77 0)
quit_daemon()

file(READ "${PORTABLE_DIR}/config.ini" portable_text)
if(NOT portable_text MATCHES "volume=77")
    message(FATAL_ERROR
        "config_check: portable config has no volume:\n${portable_text}")
endif()

# --- Atomic write leaves no temporary files ---------------------------------

file(GLOB leftovers "${PORTABLE_DIR}/config.ini.tmp.*")
if(leftovers)
    message(FATAL_ERROR
        "config_check: temporary files left behind: ${leftovers}")
endif()

message(STATUS "config_check: ok")
