# Size gate. Run as:
#   cmake -DBINARY=<path> -DLIMIT_KB=<n> -P cmake/check_size.cmake
#
# Failing here fails the milestone: AGENTS.md 1.6 makes the measured size a
# requirement, not a report.

if(NOT DEFINED BINARY OR NOT DEFINED LIMIT_KB)
    message(FATAL_ERROR "check_size: BINARY and LIMIT_KB are required")
endif()

if(NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "check_size: no such file: ${BINARY}")
endif()

file(SIZE "${BINARY}" size_bytes)
math(EXPR limit_bytes "${LIMIT_KB} * 1024")
math(EXPR size_kb "${size_bytes} / 1024")
math(EXPR used_pct "${size_bytes} * 100 / ${limit_bytes}")

message(STATUS "binary: ${BINARY}")
message(STATUS "size:   ${size_bytes} bytes (${size_kb} KB)")
message(STATUS "budget: ${limit_bytes} bytes (${LIMIT_KB} KB), ${used_pct}% used")

if(size_bytes GREATER limit_bytes)
    message(FATAL_ERROR
        "size gate failed: ${size_kb} KB exceeds the ${LIMIT_KB} KB budget")
endif()
