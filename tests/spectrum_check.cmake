# Spectrum gate, run as a ctest entry:
#   cmake -DBINARY=<exe> -DWORKDIR=<dir> -DSCRIPT=<check_spectrum.py> -DPYTHON=<exe> -P tests/spectrum_check.cmake
#
# Renders each noise type for 20 s and checks the slope and RMS against PRD
# FR-02. 20 s is a compromise: brown noise has most of its energy near DC, so
# short windows wander by a few tenths of a dB.

if(NOT DEFINED BINARY OR NOT DEFINED WORKDIR OR NOT DEFINED SCRIPT OR NOT DEFINED PYTHON)
    message(FATAL_ERROR "spectrum_check: BINARY, WORKDIR, SCRIPT and PYTHON are required")
endif()

foreach(TYPE white pink brown)
    set(RENDER "${WORKDIR}/spectrum_${TYPE}.raw")

    execute_process(
        COMMAND "${BINARY}" --render "${RENDER}" --seconds 20 --seed 42 --noise ${TYPE}
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "spectrum_check: render of ${TYPE} failed with ${result}")
    endif()

    execute_process(
        COMMAND "${PYTHON}" "${SCRIPT}" "${RENDER}" --type ${TYPE} --strict
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "spectrum_check: ${TYPE} out of tolerance\n${output}${errors}")
    endif()

    string(REPLACE "\n" "\n  " indented "${output}")
    message(STATUS "${TYPE}:\n  ${indented}")
endforeach()

# The same data through the WAV container must decode to the same spectrum, so
# a malformed header cannot pass unnoticed.
set(WAV "${WORKDIR}/spectrum_white.wav")
execute_process(
    COMMAND "${BINARY}" --render "${WAV}" --seconds 20 --seed 42 --noise white
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spectrum_check: wav render failed with ${result}")
endif()

execute_process(
    COMMAND "${PYTHON}" "${SCRIPT}" "${WAV}" --type white --strict
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spectrum_check: WAV out of tolerance\n${output}${errors}")
endif()
message(STATUS "wav: ok")
