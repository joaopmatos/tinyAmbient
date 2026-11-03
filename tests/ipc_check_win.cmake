# IPC, single instance, and forwarded commands on Windows (PRD FR-07, FR-08,
# FR-09).
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -DVERSION=<v> -P tests/ipc_check_win.cmake
#
# The POSIX harness also asserts socket modes, /proc process counts, and
# wall-clock latency. None of those exist here, so this one covers the
# observable contract: one daemon, forwarded commands, persistence, and a clean
# quit. A private pipe name keeps the run away from a real session.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR OR NOT DEFINED VERSION)
    message(FATAL_ERROR
        "ipc_check_win: BINARY, WORKDIR and VERSION are required")
endif()

set(SANDBOX "${WORKDIR}/ipc_check")
file(REMOVE_RECURSE "${SANDBOX}")
file(MAKE_DIRECTORY "${SANDBOX}")

set(APPDATA "${SANDBOX}/appdata")
set(CFG "${APPDATA}/tinyAmbient/config.ini")
file(MAKE_DIRECTORY "${APPDATA}")

string(MD5 IPC_TAG "${WORKDIR}")
set(IPC_NAME "\\\\.\\pipe\\tinyambient-ipc-${IPC_TAG}")

function(run_exe)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "APPDATA=${APPDATA}"
            "TINYAMBIENT_AUDIO=null"
            "TINYAMBIENT_IPC=${IPC_NAME}"
            "${BINARY}" ${ARGN}
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
            "ipc_check_win: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_OUT STREQUAL "")
        message(FATAL_ERROR
            "ipc_check_win: ${label}: unexpected stdout: ${TA_OUT}")
    endif()
endfunction()

function(expect_status label playing noise volume mono)
    if(NOT TA_CODE EQUAL 0)
        message(FATAL_ERROR
            "ipc_check_win: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_ERR STREQUAL "")
        message(FATAL_ERROR
            "ipc_check_win: ${label}: unexpected stderr: ${TA_ERR}")
    endif()

    set(expected
        "version=${VERSION}\nplaying=${playing}\nnoise=${noise}\nvolume=${volume}\nmono=${mono}\nstop_after=")
    string(FIND "${TA_OUT}" "${expected}" position)
    if(NOT position EQUAL 0)
        message(FATAL_ERROR
            "ipc_check_win: ${label}: status mismatch\nexpected prefix:\n${expected}\ngot:\n${TA_OUT}")
    endif()
endfunction()

# --- No daemon: status falls back to the configuration -----------------------

run_exe(--status)
expect_status("status without a daemon" 0 pink 20 0)
if(NOT TA_OUT MATCHES "stop_after=off\n$")
    message(FATAL_ERROR "ipc_check_win: no-daemon status has no stop_after=off")
endif()

# --- A bare start creates the daemon -----------------------------------------

run_exe()
expect_ok("bare start")

run_exe(--status)
expect_status("status with a daemon" 1 pink 20 0)

# A second bare invocation is a no-op: it answers from the daemon instead of
# spawning a second one (FR-07). A second daemon could not bind the pipe and
# would surface as a non-zero exit.
run_exe()
expect_ok("second bare invocation")

# --- Forwarded content commands ----------------------------------------------

run_exe(--volume 25)
expect_ok("volume over IPC")
run_exe(--status)
expect_status("volume applied" 1 pink 25 0)

run_exe(--noise brown --stereo)
expect_ok("noise and stereo over IPC")
run_exe(--status)
expect_status("noise applied" 1 brown 25 0)

run_exe(--mono)
expect_ok("mono over IPC")
run_exe(--status)
expect_status("mono applied" 1 brown 25 1)

file(READ "${CFG}" config_text)
if(NOT config_text MATCHES "noise=brown")
    message(FATAL_ERROR "ipc_check_win: forwarded noise was not persisted")
endif()
if(NOT config_text MATCHES "volume=25")
    message(FATAL_ERROR "ipc_check_win: forwarded volume was not persisted")
endif()
if(NOT config_text MATCHES "mono=true")
    message(FATAL_ERROR "ipc_check_win: forwarded mono was not persisted")
endif()

# --- Playback commands change session state only -----------------------------

run_exe(toggle)
expect_ok("toggle to pause")
run_exe(--status)
expect_status("paused" 0 brown 25 1)

run_exe(toggle)
expect_ok("toggle to play")
run_exe(--status)
expect_status("playing again" 1 brown 25 1)

run_exe(pause)
run_exe(--status)
expect_status("pause command" 0 brown 25 1)

run_exe(play)
run_exe(--status)
expect_status("play command" 1 brown 25 1)

# --- The session timer is forwarded and is not persisted ---------------------

run_exe(--stop-after 1)
expect_ok("stop-after over IPC")
run_exe(--status)
string(REGEX MATCH "stop_after=([0-9]+)" matched "${TA_OUT}")
if(NOT matched)
    message(FATAL_ERROR
        "ipc_check_win: stop-after is not a remaining count:\n${TA_OUT}")
endif()
set(remaining "${CMAKE_MATCH_1}")
if(remaining LESS 55 OR remaining GREATER 60)
    message(FATAL_ERROR
        "ipc_check_win: stop_after=${remaining}, expected 55..60")
endif()

run_exe(--stop-after off)
run_exe(--status)
if(NOT TA_OUT MATCHES "stop_after=off\n$")
    message(FATAL_ERROR "ipc_check_win: stop-after off was not applied")
endif()

file(READ "${CFG}" config_after_timer)
if(config_after_timer MATCHES "stop")
    message(FATAL_ERROR
        "ipc_check_win: the session timer reached the config file")
endif()

# --- Quit cleans up and the next status falls back ---------------------------

run_exe(quit)
expect_ok("quit over IPC")

run_exe(--status)
expect_status("status after quit" 0 brown 25 1)

# Quitting with nothing running is not an error.
run_exe(quit)
expect_ok("quit without a daemon")

# --- A start right after quit must not collide with the leaving daemon -------

foreach(round RANGE 1 5)
    run_exe()
    expect_ok("start ${round} right after quit")
    run_exe(quit)
    expect_ok("quit ${round}")
endforeach()

message(STATUS "ipc_check_win: ok")
