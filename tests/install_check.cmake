# Install layout (AGENTS.md 5). Runs `cmake --install` into a throwaway prefix
# and checks the four destinations a package manager relies on.
#   cmake -DBUILD_DIR=<build> -DWORKDIR=<build> [-DCONFIG=<cfg>] \
#         -P tests/install_check.cmake

if(NOT DEFINED BUILD_DIR OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "install_check: BUILD_DIR and WORKDIR are required")
endif()

set(root "${WORKDIR}/install_check")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")

set(install_args --install "${BUILD_DIR}" --prefix "${root}")
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND install_args --config "${CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${install_args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "install_check: cmake --install failed (${result})\n${output}${errors}")
endif()

if(WIN32)
    set(binary "bin/tinyambient.exe")
else()
    set(binary "bin/tinyambient")
endif()

foreach(relative
        "${binary}"
        "share/man/man1/tinyambient.1"
        "share/licenses/tinyambient/LICENSE"
        "share/doc/tinyambient/README.md")
    if(NOT EXISTS "${root}/${relative}")
        message(FATAL_ERROR
            "install_check: ${relative} is missing from ${root}")
    endif()
endforeach()

message(STATUS "install_check: ok")
