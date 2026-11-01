# Memory and CPU gate: how much the daemon uses while it plays.
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -DRSS_KB=<n> -DCPU_TENTHS=<n>
#         [-DSECONDS=<n>] -P tests/resource_check.cmake
#
# POSIX only, Release only: sanitizers and -O0 inflate both numbers by several
# times, so measuring them there says nothing about shipped code.
#
# CI has no sound card, so the daemon runs under the null backend. That path
# wakes more often than Pulse, which makes its CPU figure an upper bound. The
# number to quote comes from scripts/measure.sh on a real device.
#
# Budgets are 1.7x to 2x the measurement. They cannot be tighter: a clock tick
# rounds a 10 s average to 0.1%, and a shared runner adds load. Regressions
# worth catching move these numbers by an order of magnitude.
#
# Every run gets its own XDG directories, so it can never reach a real session.
#
# Teardown happens before the assertions, so a failing budget still leaves no
# daemon behind for the next test to trip over.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR OR NOT DEFINED RSS_KB
   OR NOT DEFINED CPU_TENTHS)
    message(FATAL_ERROR
        "resource_check: BINARY, WORKDIR, RSS_KB and CPU_TENTHS are required")
endif()

if(NOT DEFINED SECONDS)
    set(SECONDS 10)
endif()

set(SANDBOX "${WORKDIR}/resource_check")
file(REMOVE_RECURSE "${SANDBOX}")
file(MAKE_DIRECTORY "${SANDBOX}")

set(XDG "${SANDBOX}/xdg")
set(RUNTIME "${SANDBOX}/runtime")
set(SOCKET "${RUNTIME}/tinyambient.sock")
set(LOG "${SANDBOX}/daemon.log")
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
        TIMEOUT 30
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)

    set(TA_OUT "${output}" PARENT_SCOPE)
    set(TA_ERR "${errors}" PARENT_SCOPE)
    set(TA_CODE "${result}" PARENT_SCOPE)
endfunction()

# Reads VmRSS, VmHWM and the thread count. Peak matters as much as steady
# state: memory that grows between the warm-up and the teardown shows up in
# VmHWM and nowhere else.
macro(ta_read_status)
    file(READ "/proc/${daemon_pid}/status" ta_status_text)

    if(NOT ta_status_text MATCHES "VmRSS:[ \t]+([0-9]+)")
        message(FATAL_ERROR "resource_check: cannot read VmRSS")
    endif()
    set(rss_kb "${CMAKE_MATCH_1}")

    if(NOT ta_status_text MATCHES "VmHWM:[ \t]+([0-9]+)")
        message(FATAL_ERROR "resource_check: cannot read VmHWM")
    endif()
    set(peak_kb "${CMAKE_MATCH_1}")

    if(NOT ta_status_text MATCHES "Threads:[ \t]+([0-9]+)")
        message(FATAL_ERROR "resource_check: cannot read the thread count")
    endif()
    set(threads "${CMAKE_MATCH_1}")
endmacro()

# utime and stime are fields 14 and 15 of /proc/<pid>/stat, which count from 1.
# The command name is in parentheses and may contain spaces, so everything up to
# the last ") " is dropped before the fields are numbered.
macro(ta_read_ticks out)
    file(READ "/proc/${daemon_pid}/stat" ta_stat_text)
    string(REGEX REPLACE "^.*\\) " "" ta_stat_rest "${ta_stat_text}")
    string(REPLACE " " ";" ta_stat_fields "${ta_stat_rest}")
    list(GET ta_stat_fields 11 ta_utime)
    list(GET ta_stat_fields 12 ta_stime)
    math(EXPR ${out} "${ta_utime} + ${ta_stime}")
endmacro()

macro(ta_alive out)
    execute_process(
        COMMAND sh -c "kill -0 \"$1\" 2>/dev/null" sh "${daemon_pid}"
        RESULT_VARIABLE ta_kill_result
        ERROR_QUIET)
    if(ta_kill_result EQUAL 0)
        set(${out} 1)
    else()
        set(${out} 0)
    endif()
endmacro()

# Every early exit goes through here: a measurement that cannot be taken still
# has to stop the daemon it started.
macro(ta_fail message_text)
    if(daemon_pid MATCHES "^[0-9]+$")
        execute_process(COMMAND sh -c "kill -TERM \"$1\" 2>/dev/null" sh
            "${daemon_pid}" ERROR_QUIET)
    endif()
    message(FATAL_ERROR "resource_check: ${message_text}")
endmacro()

# --- Start the daemon --------------------------------------------------------
#
# --foreground keeps the process as a child of this script instead of detaching
# it, so its pid is the one that was just spawned. `echo $!` is the only way a
# CMake script can learn that pid.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${XDG}"
        "XDG_RUNTIME_DIR=${RUNTIME}"
        "TINYAMBIENT_AUDIO=null"
        sh -c "exec \"$1\" --noise pink --foreground >\"$2\" 2>&1 & echo $!"
        sh "${BINARY}" "${LOG}"
    TIMEOUT 30
    OUTPUT_VARIABLE daemon_pid
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT daemon_pid MATCHES "^[0-9]+$")
    ta_fail("cannot read the daemon pid")
endif()

# Readiness is the socket appearing, the same signal the IPC test uses.
set(ready 0)
foreach(attempt RANGE 1 100)
    ta_alive(alive)
    if(NOT alive)
        break()
    endif()
    if(EXISTS "${SOCKET}")
        set(ready 1)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

if(NOT ready)
    file(READ "${LOG}" log_text)
    ta_fail("the daemon did not start\n${log_text}")
endif()

# Warm-up: dlopen, the device buffers, and the first touch of every page. Sizing
# a process before that measures the loader, not the program.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)

execute_process(
    COMMAND getconf CLK_TCK
    OUTPUT_VARIABLE clk_tck
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT clk_tck MATCHES "^[0-9]+$")
    set(clk_tck 100)
endif()

# --- Measure ----------------------------------------------------------------
#
# One tick is 1/CLK_TCK of a second, so a 10 s window on a 100 Hz clock resolves
# the result to 0.1%.
ta_read_ticks(ticks_before)
ta_read_status()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${SECONDS})

ta_read_ticks(ticks_after)
ta_read_status()

math(EXPR ticks "${ticks_after} - ${ticks_before}")
math(EXPR cpu_tenths "${ticks} * 1000 / (${clk_tck} * ${SECONDS})")
math(EXPR cpu_whole "${cpu_tenths} / 10")
math(EXPR cpu_frac "${cpu_tenths} % 10")
math(EXPR budget_whole "${CPU_TENTHS} / 10")
math(EXPR budget_frac "${CPU_TENTHS} % 10")

message(STATUS "resource: peak RSS ${peak_kb} KB of ${RSS_KB} KB (steady ${rss_kb} KB)")
message(STATUS
    "resource: CPU ${cpu_whole}.${cpu_frac}% of one core of ${budget_whole}.${budget_frac}% "
    "(${ticks} ticks at ${clk_tck} Hz over ${SECONDS}s)")
message(STATUS "resource: ${threads} threads")

# --- Teardown ---------------------------------------------------------------
#
# Before the assertions: a failed budget must still leave no daemon behind.
run_exe(--quit)

set(alive 1)
foreach(attempt RANGE 1 100)
    ta_alive(alive)
    if(NOT alive)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

set(quit_clean 1)
if(alive)
    set(quit_clean 0)
    execute_process(COMMAND sh -c "kill -TERM \"$1\" 2>/dev/null" sh
        "${daemon_pid}" ERROR_QUIET)
endif()

# --- Assert -----------------------------------------------------------------

set(breach "")

if(peak_kb GREATER RSS_KB)
    string(APPEND breach
        "\n  peak RSS ${peak_kb} KB exceeds the ${RSS_KB} KB budget")
endif()

if(cpu_tenths GREATER CPU_TENTHS)
    string(APPEND breach
        "\n  CPU ${cpu_whole}.${cpu_frac}% exceeds the ${budget_whole}.${budget_frac}% budget")
endif()

if(NOT quit_clean)
    string(APPEND breach
        "\n  the daemon ignored --quit and had to be killed")
endif()

if(breach)
    message(FATAL_ERROR
        "resource gate failed:${breach}\n"
        "Raise the budget only with the measurement that justifies it, in the "
        "same diff (AGENTS.md 6.6 and 6.10).")
endif()

message(STATUS "resource_check: ok")
