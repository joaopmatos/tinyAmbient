# Render checks, run as a ctest entry:
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -P tests/render_check.cmake
#
# Two invariants from PRD section 10:
#   FR-10  file size is 384000 * seconds bytes
#   NFR-06 the same seed produces byte-identical output
#
# A DSP that reads uninitialized memory or the wall clock fails here, which is
# the class of bug a listening test cannot catch.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "render_check: BINARY and WORKDIR are required")
endif()

set(FIRST "${WORKDIR}/render_check_a.raw")
set(SECOND "${WORKDIR}/render_check_b.raw")

foreach(SECONDS 1 3)
    execute_process(
        COMMAND "${BINARY}" --render "${FIRST}" --seconds ${SECONDS} --seed 42
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "render_check: render failed with ${result}")
    endif()

    file(SIZE "${FIRST}" size_bytes)
    math(EXPR expected "${SECONDS} * 384000")
    if(NOT size_bytes EQUAL expected)
        message(FATAL_ERROR
            "render_check: ${SECONDS}s render is ${size_bytes} bytes, expected ${expected}")
    endif()
endforeach()

execute_process(
    COMMAND "${BINARY}" --render "${FIRST}" --seconds 2 --seed 7
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: render failed with ${result}")
endif()
execute_process(
    COMMAND "${BINARY}" --render "${SECOND}" --seconds 2 --seed 7
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: render failed with ${result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${FIRST}" "${SECOND}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: same seed produced different bytes")
endif()

# A different seed must produce different bytes, otherwise the seed is ignored.
execute_process(
    COMMAND "${BINARY}" --render "${SECOND}" --seconds 2 --seed 8
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: render failed with ${result}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${FIRST}" "${SECOND}"
    RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "render_check: seed change had no effect")
endif()

message(STATUS "render_check: ok")
