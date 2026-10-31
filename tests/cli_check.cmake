# Exit-code and diagnostic contract (PRD FR-14).
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -P tests/cli_check.cmake
#
# The parser has its own unit test; this one covers the layer above it, where
# usage errors, runtime failures, and the "tinyambient: " prefix on stderr are
# the observable contract.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "cli_check: BINARY and WORKDIR are required")
endif()

set(OUT_RAW "${WORKDIR}/cli_check.raw")

function(expect_code expected label)
    execute_process(
        COMMAND "${BINARY}" ${ARGN}
        RESULT_VARIABLE result
        TIMEOUT 60
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)

    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "cli_check: ${label}: exit ${result}, expected ${expected}\n${output}${errors}")
    endif()

    # Failures must be diagnosable: prefix, one line, nothing on stdout.
    if(NOT expected EQUAL 0)
        if(NOT errors MATCHES "^tinyambient: ")
            message(FATAL_ERROR
                "cli_check: ${label}: stderr lacks the 'tinyambient: ' prefix: ${errors}")
        endif()
        if(NOT output STREQUAL "")
            message(FATAL_ERROR
                "cli_check: ${label}: wrote to stdout on failure: ${output}")
        endif()
    endif()
endfunction()

# Success paths
expect_code(0 "--version" --version)
expect_code(0 "--help" --help)
expect_code(0 "help wins over a bad flag" --bogus --help)
expect_code(0 "version wins over a bad value" --volume 101 --version)
expect_code(0 "render to a file" --render "${OUT_RAW}" --seconds 1)
expect_code(0 "render is case-insensitive for .wav"
    --render "${WORKDIR}/cli_check.WAV" --seconds 1)
expect_code(0 "render with a volume"
    --render "${OUT_RAW}" --seconds 1 --volume 0)
expect_code(0 "mono render" --render "${OUT_RAW}" --seconds 1 --mono)

# Usage errors: exit 2
expect_code(2 "unknown option" --bogus)
expect_code(2 "unknown short option" -v)
expect_code(2 "noise without a value" noise)
expect_code(2 "volume without a value" --volume)
expect_code(2 "bad noise type" --render "${OUT_RAW}" --noise gray)
expect_code(2 "bad noise type as a setting" --noise gray)
expect_code(2 "volume above range" --volume 101)
expect_code(2 "stop-after below range" --stop-after 0)
expect_code(2 "bad stop-after value" --stop-after soon)
expect_code(2 "seconds below range" --render "${OUT_RAW}" --seconds 0)
expect_code(2 "seconds above range" --render "${OUT_RAW}" --seconds 3601)
expect_code(2 "seconds without render" --seconds 5)
expect_code(2 "seed without render" --seed 5)
expect_code(2 "status with a content command" --status --volume 5)
expect_code(2 "render with a daemon command" --render "${OUT_RAW}" --play)
expect_code(2 "render with status" --render "${OUT_RAW}" --status)
expect_code(2 "render path may not be a flag" --render --help --seconds 1)
expect_code(2 "render path may not start with a dash"
    --render -out.raw --seconds 1)
expect_code(2 "duplicate render"
    --render "${OUT_RAW}" --render "${WORKDIR}/cli_check_b.raw" --seconds 1)
expect_code(2 "duplicate seconds" --render "${OUT_RAW}" --seconds 1 --seconds 2)
expect_code(2 "duplicate seed" --render "${OUT_RAW}" --seed 1 --seed 2)
expect_code(2 "trailing argument after --" -- --bogus)

# Runtime failures: exit 1
expect_code(1 "unwritable render path"
    --render "${WORKDIR}/no-such-directory/out.raw" --seconds 1)

message(STATUS "cli_check: ok")
