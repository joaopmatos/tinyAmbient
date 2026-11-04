# IPC, single instance, and forwarded commands (PRD FR-07, FR-08, FR-09).
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -DVERSION=<v> -P tests/ipc_check.cmake
#
# POSIX only. Every run gets its own XDG_RUNTIME_DIR and XDG_CONFIG_HOME, so the
# daemon in this test can never reach the developer's real session.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR OR NOT DEFINED VERSION)
    message(FATAL_ERROR
        "ipc_check: BINARY, WORKDIR and VERSION are required")
endif()

set(SANDBOX "${WORKDIR}/ipc_check")
file(REMOVE_RECURSE "${SANDBOX}")
file(MAKE_DIRECTORY "${SANDBOX}")

set(XDG "${SANDBOX}/xdg")
set(RUNTIME "${SANDBOX}/runtime")
set(SOCKET "${RUNTIME}/tinyambient.sock")
set(CFG "${XDG}/tinyambient/config.ini")
file(MAKE_DIRECTORY "${XDG}")
file(MAKE_DIRECTORY "${RUNTIME}")

function(run_exe)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_CONFIG_HOME=${XDG}"
            "XDG_RUNTIME_DIR=${RUNTIME}"
            "TINYAMBIENT_AUDIO=null"
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
            "ipc_check: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_OUT STREQUAL "")
        message(FATAL_ERROR
            "ipc_check: ${label}: unexpected stdout: ${TA_OUT}")
    endif()
endfunction()

function(expect_status label playing noise volume mono)
    if(NOT TA_CODE EQUAL 0)
        message(FATAL_ERROR
            "ipc_check: ${label}: exit ${TA_CODE}\n${TA_OUT}${TA_ERR}")
    endif()
    if(NOT TA_ERR STREQUAL "")
        message(FATAL_ERROR
            "ipc_check: ${label}: unexpected stderr: ${TA_ERR}")
    endif()

    set(expected
        "version=${VERSION}\nplaying=${playing}\nnoise=${noise}\nvolume=${volume}\nmono=${mono}\nstop_after=")
    string(FIND "${TA_OUT}" "${expected}" position)
    if(NOT position EQUAL 0)
        message(FATAL_ERROR
            "ipc_check: ${label}: status mismatch\nexpected prefix:\n${expected}\ngot:\n${TA_OUT}")
    endif()
endfunction()

# Counts live processes whose executable is this binary. The client exits before
# the count runs, so a non-zero number means a daemon is alive.
function(count_daemons out)
    execute_process(
        COMMAND sh -c
            "n=0; for p in /proc/[0-9]*; do if [ \"$(readlink -f \"$p/exe\" 2>/dev/null)\" = \"$1\" ]; then n=$((n+1)); fi; done; echo $n"
            sh "${BINARY}"
        OUTPUT_VARIABLE count
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out} "${count}" PARENT_SCOPE)
endfunction()

# Waits for the detached daemon to finish exiting. The quit reply is sent
# before the loop tears down the audio device, so the process can outlive the
# client by a few milliseconds.
function(wait_no_daemons label)
    foreach(attempt RANGE 1 60)
        count_daemons(count)
        if(count EQUAL 0)
            return()
        endif()
        execute_process(COMMAND sleep 0.05)
    endforeach()
    message(FATAL_ERROR "ipc_check: ${label}: daemons did not exit")
endfunction()

# --- No daemon: status falls back to the configuration and starts nothing ----

run_exe(--status)
expect_status("status without a daemon" 0 pink 20 0)
if(NOT TA_OUT MATCHES "stop_after=off\n$")
    message(FATAL_ERROR "ipc_check: no-daemon status has no stop_after=off")
endif()
if(EXISTS "${SOCKET}")
    message(FATAL_ERROR "ipc_check: --status created a daemon socket")
endif()

# A bare start creates the daemon, plays the saved settings, and exits 0
# (FR-09).
run_exe()
expect_ok("bare start")
if(NOT EXISTS "${SOCKET}")
    message(FATAL_ERROR "ipc_check: bare start created no socket")
endif()

execute_process(COMMAND stat -c %a "${SOCKET}"
    OUTPUT_VARIABLE socket_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT socket_mode STREQUAL "600")
    message(FATAL_ERROR
        "ipc_check: socket mode is ${socket_mode}, expected 600")
endif()

run_exe(--status)
expect_status("status with a daemon" 1 pink 20 0)

# A second bare invocation is a no-op and must not create a second daemon
# (FR-07).
run_exe()
expect_ok("second bare invocation")
count_daemons(count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "ipc_check: ${count} daemons after two starts")
endif()

# --- Forwarded content commands ---------------------------------------------

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
    message(FATAL_ERROR "ipc_check: forwarded noise was not persisted")
endif()
if(NOT config_text MATCHES "volume=25")
    message(FATAL_ERROR "ipc_check: forwarded volume was not persisted")
endif()
if(NOT config_text MATCHES "mono=true")
    message(FATAL_ERROR "ipc_check: forwarded mono was not persisted")
endif()

# --- Playback commands change session state only ----------------------------

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

# --- The session timer is forwarded and is not persisted --------------------

run_exe(--stop-after 1)
expect_ok("stop-after over IPC")
run_exe(--status)
string(REGEX MATCH "stop_after=([0-9]+)" matched "${TA_OUT}")
if(NOT matched)
    message(FATAL_ERROR
        "ipc_check: stop-after is not a remaining count:\n${TA_OUT}")
endif()
set(remaining "${CMAKE_MATCH_1}")
if(remaining LESS 55 OR remaining GREATER 60)
    message(FATAL_ERROR
        "ipc_check: stop_after=${remaining}, expected 55..60")
endif()

run_exe(--stop-after off)
run_exe(--status)
if(NOT TA_OUT MATCHES "stop_after=off\n$")
    message(FATAL_ERROR "ipc_check: stop-after off was not applied")
endif()

file(READ "${CFG}" config_after_timer)
if(config_after_timer MATCHES "stop")
    message(FATAL_ERROR "ipc_check: the session timer reached the config file")
endif()

# --- Daemon-side validation and the error protocol --------------------------
#
# The CLI parser already rejects unusable commands, so the error path is
# exercised directly on the socket. The reply carries the client exit code.
execute_process(
    COMMAND python3 -c
        "import socket,sys\ns=socket.socket(socket.AF_UNIX)\ns.connect(sys.argv[1])\ns.sendall(b'render out.raw\\n')\ns.shutdown(socket.SHUT_WR)\nsys.stdout.write(s.recv(512).decode())"
        "${SOCKET}"
    RESULT_VARIABLE py_result
    OUTPUT_VARIABLE py_output)

if(NOT py_result EQUAL 0)
    message(FATAL_ERROR "ipc_check: cannot talk to the socket directly")
endif()
if(NOT py_output MATCHES "^error 2 ")
    message(FATAL_ERROR
        "ipc_check: daemon error protocol mismatch: ${py_output}")
endif()

# --- FR-07 latency: a forwarded command returns well under 100 ms -----------

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${XDG}"
        "XDG_RUNTIME_DIR=${RUNTIME}"
        "TINYAMBIENT_AUDIO=null"
        sh -c "start=$(date +%s%N); \"$1\" --status >/dev/null; end=$(date +%s%N); echo $(( (end - start) / 1000000 ))"
        sh "${BINARY}"
    OUTPUT_VARIABLE elapsed
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT elapsed MATCHES "^[0-9]+$")
    message(FATAL_ERROR "ipc_check: cannot measure latency: ${elapsed}")
endif()
if(elapsed GREATER 100)
    message(FATAL_ERROR
        "ipc_check: a forwarded status took ${elapsed} ms, budget is 100")
endif()

# --- Quit cleans up and the next status falls back --------------------------

run_exe(quit)
expect_ok("quit over IPC")
if(EXISTS "${SOCKET}")
    message(FATAL_ERROR "ipc_check: quit left the socket behind")
endif()

wait_no_daemons("quit")
count_daemons(count)
if(NOT count EQUAL 0)
    message(FATAL_ERROR "ipc_check: ${count} daemons after quit")
endif()

run_exe(--status)
expect_status("status after quit" 0 brown 25 1)

# Quitting with nothing running is not an error: nothing was there to stop.
run_exe(quit)
expect_ok("quit without a daemon")

# --- A start right after quit must not collide with the leaving daemon -------
#
# The reply is the client's signal that the lock is free. The daemon used to
# release it only after stopping the audio device, so a back-to-back restart was
# refused with exit 3 whenever the teardown outlived the next process spawn.
foreach(round RANGE 1 5)
    run_exe()
    expect_ok("start ${round} right after quit")
    run_exe(quit)
    expect_ok("quit ${round}")
endforeach()

# --- --foreground participates in the single-instance contract ---------------
#
# A second foreground start must fail on the address before it persists
# anything: the running daemon would never see the saved setting.

run_exe()
expect_ok("start before the foreground checks")
run_exe(--status)
expect_status("daemon before the foreground checks" 1 brown 25 1)

file(READ "${CFG}" config_before_foreground)
run_exe(--foreground --volume 50)
if(NOT TA_CODE EQUAL 3)
    message(FATAL_ERROR
        "ipc_check: --foreground against a daemon: exit ${TA_CODE}, expected 3")
endif()
if(NOT TA_ERR MATCHES "another daemon is already running")
    message(FATAL_ERROR
        "ipc_check: --foreground conflict message: ${TA_ERR}")
endif()
file(READ "${CFG}" config_after_foreground)
if(NOT config_after_foreground STREQUAL config_before_foreground)
    message(FATAL_ERROR
        "ipc_check: --foreground conflict rewrote the configuration")
endif()
run_exe(--status)
expect_status("daemon untouched by the conflict" 1 brown 25 1)

# quit with --foreground reaches the running daemon; with none it is satisfied
# without starting one.
run_exe(--foreground --quit)
expect_ok("foreground quit over IPC")
if(EXISTS "${SOCKET}")
    message(FATAL_ERROR "ipc_check: foreground quit left the socket behind")
endif()
wait_no_daemons("foreground quit")
run_exe(--status)
expect_status("status after the foreground quit" 0 brown 25 1)

run_exe(--foreground --quit)
expect_ok("foreground quit without a daemon")
run_exe(--status)
expect_status("status after the no-op foreground quit" 0 brown 25 1)

message(STATUS "ipc_check: ok")
