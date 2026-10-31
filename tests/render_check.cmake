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

# --- Volume 0 is exact silence (PRD FR-03) ----------------------------------

set(SILENT "${WORKDIR}/render_check_silent.raw")
execute_process(
    COMMAND "${BINARY}" --render "${SILENT}" --seconds 1 --volume 0
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: volume-0 render failed with ${result}")
endif()

file(SIZE "${SILENT}" silent_size)
if(NOT silent_size EQUAL 384000)
    message(FATAL_ERROR
        "render_check: volume-0 render is ${silent_size} bytes, expected 384000")
endif()

file(READ "${SILENT}" silent_hex HEX)
if(NOT silent_hex MATCHES "^0+$")
    message(FATAL_ERROR
        "render_check: volume 0 produced a non-zero sample")
endif()

# --- WAV container (PRD FR-10) ----------------------------------------------

set(WAV "${WORKDIR}/render_check.wav")
set(WAV_SECONDS 1)
execute_process(
    COMMAND "${BINARY}" --render "${WAV}" --seconds ${WAV_SECONDS} --seed 42
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "render_check: wav render failed with ${result}")
endif()

math(EXPR wav_data "${WAV_SECONDS} * 384000")
math(EXPR wav_total "${wav_data} + 44")

file(SIZE "${WAV}" wav_size)
if(NOT wav_size EQUAL wav_total)
    message(FATAL_ERROR
        "render_check: wav is ${wav_size} bytes, expected ${wav_total} (44 header + ${wav_data})")
endif()

file(READ "${WAV}" header OFFSET 0 LIMIT 44 HEX)

# Little-endian, so 48000 is 80bb0000 and 384000 is 00dc0500.
function(expect_hex offset bytes expected label)
    math(EXPR start "${offset} * 2")
    math(EXPR length "${bytes} * 2")
    string(SUBSTRING "${header}" ${start} ${length} got)
    if(NOT got STREQUAL expected)
        message(FATAL_ERROR
            "render_check: wav ${label} at offset ${offset} is ${got}, expected ${expected}")
    endif()
endfunction()

expect_hex(0 4 52494646 "RIFF tag")
expect_hex(8 4 57415645 "WAVE tag")
expect_hex(12 4 666d7420 "fmt tag")
expect_hex(16 4 10000000 "fmt chunk size")
expect_hex(20 2 0300 "audio format (IEEE float)")
expect_hex(22 2 0200 "channel count")
expect_hex(24 4 80bb0000 "sample rate")
expect_hex(28 4 00dc0500 "byte rate")
expect_hex(32 2 0800 "block align")
expect_hex(34 2 2000 "bits per sample")
expect_hex(36 4 64617461 "data tag")
expect_hex(40 4 00dc0500 "data size")

message(STATUS "render_check: ok")
