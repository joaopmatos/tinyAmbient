cmake_minimum_required(VERSION 3.20)

project(tinyAmbient
    VERSION 1.0.0
    DESCRIPTION "Tiny ambient noise player"
    LANGUAGES C)

# --- Language settings -------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)   # -std=c11, not gnu11

# Static CRT on Windows (/MT): no VC++ Redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TA_SANITIZE "Build own code with ASan and UBSan" OFF)

# --- Size budget (AGENTS.md 1.6) --------------------------------------------

if(WIN32)
    set(TA_SIZE_LIMIT_KB 300)
else()
    set(TA_SIZE_LIMIT_KB 150)
endif()

# --- Flag helpers ------------------------------------------------------------
#
# Release flags follow AGENTS.md section 4. -ffunction-sections/-fdata-sections
# put every function and variable in its own section; --gc-sections then drops
# the ones nothing references. Without that pair, linking miniaudio would pull
# in the whole backends list even though v1 only calls the playback path.
#
# -flto=auto instead of plain -flto: same optimisation, but GCC parallelises the
# LTRANS jobs and stops emitting "using serial compilation" on every link.

function(ta_optimize target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O1>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Debug>:/Od>
            $<$<CONFIG:Debug>:/RTC1>)
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
    else()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-Os>
            $<$<CONFIG:Release>:-flto=auto>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
            $<$<CONFIG:Release>:-fno-asynchronous-unwind-tables>
            $<$<CONFIG:Release>:-fno-unwind-tables>
            $<$<CONFIG:Release>:-fno-ident>
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-g>)
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:-flto=auto>
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-s>)
    endif()
endfunction()

function(ta_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        # C4996 flags every ISO C CRT call (fopen, sscanf) as deprecated in
        # favour of Annex K, which glibc does not implement. Keeping the
        # portable calls means the same source is exercised on both platforms;
        # AGENTS.md 5 already bans the genuinely unsafe ones (strcpy, sprintf).
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# MSVC compiles <stdatomic.h> only with an explicit opt-in: without
# /experimental:c11atomics the header stops with "C atomic support is not
# enabled". CMAKE_C_STANDARD 11 asks for /std:c11 but does not imply the atomic
# flag, so both are set here for every target built from own code.
function(ta_c11 target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /std:c11
            /experimental:c11atomics)
    endif()
endfunction()

# ASan/UBSan cover own code only. ta_audio contains the miniaudio implementation
# and is deliberately left uninstrumented (AGENTS.md 1.5, PRD NFR-05). Linking an
# uninstrumented TU into an instrumented binary is supported; it just means that
# code is not checked.
function(ta_sanitize target)
    if(NOT TA_SANITIZE)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    endif()
endfunction()

# --- Layers ------------------------------------------------------------------
#
# Dependency direction is enforced by the link graph: core/ links nothing but
# libc/libm, and only platform/ may call OS APIs.

add_library(ta_core STATIC
    src/core/noise.c
    src/core/engine.c
    src/core/command.c
    src/core/config.c
    src/core/status.c)
target_include_directories(ta_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# PUBLIC so the daemon, the client, and --version report one version string
# (PRD FR-15). core/status.c formats it without touching any OS API.
target_compile_definitions(ta_core PUBLIC TA_VERSION="${PROJECT_VERSION}")
ta_warnings(ta_core)
ta_c11(ta_core)
ta_sanitize(ta_core)
ta_optimize(ta_core)

# libm is allowed in core/ (AGENTS.md 1.3) and is needed once filters and the
# gain calibration land.
if(UNIX)
    target_link_libraries(ta_core PUBLIC m)
endif()

# platform/ owns every OS API. core/ stays portable, so the config parser and
# formatter live there and the atomic write lives here.
add_library(ta_platform STATIC
    src/platform/paths.c
    src/platform/config_file.c
    src/platform/ipc.c
    src/platform/run.c)
target_link_libraries(ta_platform PUBLIC ta_core)
if(WIN32)
    # OpenProcessToken, GetTokenInformation, and ConvertSidToStringSidA: the IPC
    # pipe's per-user name and DACL.
    target_link_libraries(ta_platform PUBLIC advapi32)
endif()
ta_warnings(ta_platform)
ta_c11(ta_platform)
ta_sanitize(ta_platform)
ta_optimize(ta_platform)

add_library(ta_ui STATIC
    src/ui/cli.c)
target_link_libraries(ta_ui PUBLIC ta_core ta_platform)
ta_warnings(ta_ui)
ta_c11(ta_ui)
ta_sanitize(ta_ui)
ta_optimize(ta_ui)

add_library(ta_audio STATIC
    src/audio/device.c)
target_include_directories(ta_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM must directly follow the target name: in any other position CMake
# treats it as an include path named "SYSTEM".
target_include_directories(ta_audio SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(ta_audio PUBLIC ta_core)
ta_warnings(ta_audio)
ta_c11(ta_audio)
ta_optimize(ta_audio)

# miniaudio needs libdl (to dlopen ALSA/Pulse), pthreads, and libm on Linux.
# libasound and libpulse are deliberately absent from this list.
find_package(Threads REQUIRED)
target_link_libraries(ta_audio PUBLIC Threads::Threads)
if(UNIX)
    target_link_libraries(ta_audio PUBLIC ${CMAKE_DL_LIBS} m)
endif()

# The daemon loop (platform/run.c) starts and supervises the device, so the
# platform layer depends on audio/. Declared after ta_audio exists: CMake
# resolves link targets at generate time, but keeping the definition order
# obvious costs nothing. audio/ never links back to platform/.
target_link_libraries(ta_platform PUBLIC ta_audio)

add_executable(tinyambient
    src/main.c)
target_link_libraries(tinyambient PRIVATE ta_ui ta_platform ta_core ta_audio)
ta_warnings(tinyambient)
ta_c11(tinyambient)
ta_sanitize(tinyambient)
ta_optimize(tinyambient)

# Own code is instrumented; the binary still needs the sanitizer runtime at link
# time. ta_audio stays out of the compile side, but not out of the link line.
if(TA_SANITIZE AND NOT MSVC)
    target_link_options(tinyambient PRIVATE -fsanitize=address,undefined)
elseif(TA_SANITIZE AND MSVC)
    target_link_options(tinyambient PRIVATE /fsanitize=address)
endif()

# --- Size measurement --------------------------------------------------------
#
# Recorded at every milestone (AGENTS.md 6.6). Both a manual target and a ctest
# entry, so CI fails on breach without extra workflow steps.

set(TA_SIZE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_size.cmake)

add_custom_target(size-check
    COMMAND ${CMAKE_COMMAND}
        -DBINARY=$<TARGET_FILE:tinyambient>
        -DLIMIT_KB=${TA_SIZE_LIMIT_KB}
        -P ${TA_SIZE_SCRIPT}
    DEPENDS tinyambient
    COMMENT "Checking binary size against the ${TA_SIZE_LIMIT_KB} KB budget"
    VERBATIM)

enable_testing()

# The budget applies to the stripped Release binary. A sanitizer build links the
# ASan/UBSan runtime, which alone pushes the file past 150 KB, so measuring it
# would report a breach that says nothing about shipped code. Debug and
# RelWithDebInfo are equally unrepresentative: they skip -Os, --gc-sections, and
# stripping. The gate therefore runs only for the Release configuration.
# Multi-config generators decide at ctest time instead; run them with
# `ctest -C Release`. The manual size-check target stays available, but it too
# expects a Release configuration.
if(NOT TA_SANITIZE AND (CMAKE_CONFIGURATION_TYPES OR
                        CMAKE_BUILD_TYPE STREQUAL "Release"))
    add_test(NAME size
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:tinyambient>
            -DLIMIT_KB=${TA_SIZE_LIMIT_KB}
            -P ${TA_SIZE_SCRIPT})
endif()

# --- Unit tests --------------------------------------------------------------

function(ta_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ta_core)
    ta_warnings(${name})
    ta_c11(${name})
    ta_sanitize(${name})
    ta_optimize(${name})
    if(TA_SANITIZE AND NOT MSVC)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ta_add_test(test_noise tests/test_noise.c)
ta_add_test(test_command tests/test_command.c)
ta_add_test(test_config tests/test_config.c)
ta_add_test(test_engine tests/test_engine.c)
ta_add_test(test_status tests/test_status.c)

# Audio device path. The null backend drives the callback in real time without
# hardware, so this runs in CI; it is the only automated coverage of audio/.
add_executable(play_check tests/play_check.c)
target_include_directories(play_check SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(play_check PRIVATE ta_audio)
ta_warnings(play_check)
ta_c11(play_check)
ta_sanitize(play_check)
ta_optimize(play_check)
if(TA_SANITIZE AND NOT MSVC)
    target_link_options(play_check PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME playback COMMAND play_check 600)
set_tests_properties(playback PROPERTIES ENVIRONMENT "TINYAMBIENT_AUDIO=null")

# Run loop: stop-after expiry, the engine pausing itself, and a clean quit
# (PRD FR-05). POSIX only: the harness needs a second thread to observe a loop
# that is supposed to keep running, and MSVC has no C11 threads.
if(UNIX)
    add_executable(session_check tests/session_check.c)
    target_link_libraries(session_check PRIVATE ta_platform Threads::Threads)
    ta_warnings(session_check)
    ta_c11(session_check)
    ta_sanitize(session_check)
    ta_optimize(session_check)
    if(TA_SANITIZE AND NOT MSVC)
        target_link_options(session_check PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME session COMMAND session_check)
    set_tests_properties(session PROPERTIES ENVIRONMENT "TINYAMBIENT_AUDIO=null")
endif()

# End-to-end render checks: byte count and determinism (PRD FR-10, NFR-06).
add_test(NAME render
    COMMAND ${CMAKE_COMMAND}
        -DBINARY=$<TARGET_FILE:tinyambient>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_check.cmake)

# Exit codes and the diagnostic prefix (PRD FR-14).
add_test(NAME cli
    COMMAND ${CMAKE_COMMAND}
        -DBINARY=$<TARGET_FILE:tinyambient>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_check.cmake)

# Config persistence, status output, and portable mode (PRD FR-06, FR-08,
# FR-11, FR-13). Runs against a sandboxed XDG_CONFIG_HOME/APPDATA. The expected
# version is passed in from the project so a bump cannot desync the test.
add_test(NAME config
    COMMAND ${CMAKE_COMMAND}
        -DBINARY=$<TARGET_FILE:tinyambient>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -DVERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_check.cmake)

# IPC, single instance, and forwarded commands (PRD FR-07, FR-08, FR-09).
# Two harnesses: the POSIX one also inspects /proc, socket modes, and wall-clock
# latency; the Windows one asserts the observable contract over a private pipe.
if(UNIX)
    set(TA_IPC_CHECK tests/ipc_check.cmake)
else()
    set(TA_IPC_CHECK tests/ipc_check_win.cmake)
endif()
add_test(NAME ipc
    COMMAND ${CMAKE_COMMAND}
        -DBINARY=$<TARGET_FILE:tinyambient>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -DVERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/${TA_IPC_CHECK})

# Spectrum and RMS gate for every noise type (PRD FR-02). Python is optional to
# build but required to verify the product, so a missing interpreter is a
# configuration error unless explicitly waived: a silently skipped gate is worse
# than no gate.
option(TA_REQUIRE_SPECTRUM "Fail configure when Python3 is missing" ON)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME spectrum
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:tinyambient>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/check_spectrum.py
            -DPYTHON=${Python3_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/spectrum_check.cmake)
elseif(TA_REQUIRE_SPECTRUM)
    message(FATAL_ERROR
        "Python3 is required for the FR-02 spectrum test. Install python3, or "
        "configure with -DTA_REQUIRE_SPECTRUM=OFF to skip it deliberately.")
else()
    message(WARNING "Python3 not found: the spectrum test is disabled")
endif()
