# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt: game-independent runtime skeleton (OS/HLE layer) for the Metroid Prime Remastered static recompilation.
# Build: scripts\build.ps1   (clang-cl + Ninja, Release)   Test: ctest --test-dir build\release
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(nxrt C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "nxrt targets native Windows x64 only")
endif()

option(NXRT_BUILD_TESTS "Build the nxrt_tests executable and register its CTest entries" ON)

# The generated-code ABI header (Ctx, call_indirect, ...) lives in the recompiler (recomp/include/nxrecomp/).
# Found next to the checkout (../recomp) or, for a git worktree under runtime-wt/<name>, two levels up (../../recomp).
set(NXRT_RECOMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/include" CACHE PATH "Directory that holds nxrecomp/")
if(NOT EXISTS "${NXRT_RECOMP_INCLUDE}/nxrecomp/ctx.h" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include/nxrecomp/ctx.h")
    get_filename_component(_nxrt_ri "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include" ABSOLUTE)
    set(NXRT_RECOMP_INCLUDE "${_nxrt_ri}" CACHE PATH "Directory that holds nxrecomp/" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(/W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                        -Wno-cast-function-type-strict -Wno-language-extension-token)
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)

option(NXRT_BUILD_GFX "Build the graphics shell (SDL3 + Vulkan + NVN front end, src/gfx, cmake/gfx.cmake)" ON)
# Game modules (games/<id>/, docs/GAME_BOUNDARY.md): the title-specific code a game exe links. The test executables register one of them, so
# the tests of the base run against that game's tables (title facts, sites, probes, hooks) as the game exe does.
option(NXRT_BUILD_GAMES "Build the game modules in games/" ON)
set(NXRT_GAMES "ObeliskChamber" CACHE STRING "ids of the game modules to build (games/<id>/), a list")
set(NXRT_TEST_GAME "ObeliskChamber" CACHE STRING "id of the game module the test executables register (empty: none)")
# The module was called fefw before the project was renamed: a build folder or script that still passes the old id gets the same module.
foreach(_v NXRT_GAMES NXRT_TEST_GAME NXRT_LAUNCHER_GAME)
    if(DEFINED ${_v})
        list(TRANSFORM ${_v} REPLACE "^fefw$" "ObeliskChamber")
    endif()
endforeach()
if(NOT NXRT_BUILD_GAMES OR NOT NXRT_TEST_GAME IN_LIST NXRT_GAMES)
    set(NXRT_TEST_GAME "")      # the test game must be one of the modules built
endif()

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/gfx/")   # the graphics shell is its own library (cmake/gfx.cmake)
add_library(nxrt STATIC ${NXRT_SOURCES})
target_include_directories(nxrt PUBLIC include)
if(EXISTS "${NXRT_RECOMP_INCLUDE}")
    target_include_directories(nxrt PUBLIC "${NXRT_RECOMP_INCLUDE}")
endif()
target_link_libraries(nxrt PUBLIC psapi)
include(cmake/media_deps.cmake)   # libvpx for the Switch 2 video decoders (optional)

if(NXRT_BUILD_TESTS)
    enable_testing()
    file(GLOB NXRT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    # The tests of the test game's own runtime-side code (games/<id>/tests/core) run in the same executable.
    if(NXRT_TEST_GAME)
        file(GLOB NXRT_GAME_CORE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${NXRT_TEST_GAME}/tests/core/*.cpp")
        list(APPEND NXRT_TEST_SOURCES ${NXRT_GAME_CORE_TEST_SOURCES})
    endif()
    add_executable(nxrt_tests ${NXRT_TEST_SOURCES})
    target_include_directories(nxrt_tests PRIVATE tests)
    # nxrt calls into the graphics shell (switch2.cpp: HDR metadata, tt_core.cpp: screenshot writes), so the tests link it when it is built
    target_link_libraries(nxrt_tests PRIVATE nxrt $<TARGET_NAME_IF_EXISTS:nxrt_gfx>)
    target_compile_definitions(nxrt_tests PRIVATE NXRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    # A linker map next to the exe: the crash handler and the watchdog name host code from it. Keep host allocations low
    # (HIGHENTROPYVA:NO) so the guest ranges at 0x71_0000_0000 and up are free; a 64 MiB main stack.
    target_link_options(nxrt_tests PRIVATE /MAP /STACK:67108864 /HIGHENTROPYVA:NO /DEBUG)

    # One CTest entry per test group; the group names are listed by `nxrt_tests --list`. The list is read at configure
    # time from the NXTEST_GROUP lines of the test sources so that CMake needs no edit when a group is added.
    set(NXRT_GROUPS "")
    foreach(src ${NXRT_TEST_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP\\(")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP\\(([a-z0-9_]+)\\).*$" "\\1" g "${l}")
            list(APPEND NXRT_GROUPS ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES NXRT_GROUPS)
    foreach(g ${NXRT_GROUPS})
        add_test(NAME ${g} COMMAND nxrt_tests ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT 120)
    endforeach()
    message(STATUS "nxrt test groups: ${NXRT_GROUPS}")

    # Lifted-code integration tests (tests/lift): need the recompiler, its Python environment and the AArch64 clang.
    option(NXRT_BUILD_LIFT_TESTS "Lift test programs with nxrecomp and run them on the runtime (CTest entries lift_*)" ON)
    if(NXRT_BUILD_LIFT_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/cmake/NxRecomp.cmake" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/.venv/Scripts/python.exe")
        add_subdirectory(tests/lift)
    else()
        message(STATUS "nxrt: lifted-code integration tests (tests/lift) are off: ../recomp or ../tools/.venv not found")
    endif()
endif()

if(NXRT_BUILD_GFX)
    include(cmake/gfx.cmake)
endif()

if(NXRT_BUILD_GAMES)
    foreach(_game ${NXRT_GAMES})
        add_subdirectory(games/${_game})
    endforeach()
endif()
# The launcher's branding and factory settings: the data part of one game module and its launcher.rc (version info, manifest). Without that
# module the launcher builds unbranded, with no resources.
if(TARGET mpr_launcher AND TARGET nxrt_game_${NXRT_LAUNCHER_GAME}_data)
    target_sources(mpr_launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/${NXRT_LAUNCHER_GAME}/launcher.rc")
    target_link_libraries(mpr_launcher PRIVATE nxrt_game_${NXRT_LAUNCHER_GAME}_data)
    target_compile_definitions(mpr_launcher PRIVATE NXRT_LAUNCHER_GAME_REGISTER=nxrt_register_game_${NXRT_LAUNCHER_GAME}_data)
    target_link_options(mpr_launcher PRIVATE /MANIFEST:NO)   # launcher.rc carries the manifest
endif()
# Each test executable registers the most of the test game it links: the data part (launcher tests), the core part (runtime tests), the whole
# module (graphics tests). tests/test_main.cpp calls NXRT_TEST_GAME_REGISTER before the first test.
if(NXRT_BUILD_TESTS AND NXRT_TEST_GAME)
    set(_g "${NXRT_TEST_GAME}")
    foreach(pair "nxrt_tests;nxrt_game_${_g}_core;nxrt_register_game_${_g}_core" "nxrt_gfx_tests;nxrt_game_${_g};nxrt_register_game_${_g}"
                 "mpr_launcher_tests;nxrt_game_${_g}_data;nxrt_register_game_${_g}_data")
        list(GET pair 0 exe)
        list(GET pair 1 lib)
        list(GET pair 2 fn)
        if(TARGET ${exe} AND TARGET ${lib})
            target_link_libraries(${exe} PRIVATE ${lib})
            target_compile_definitions(${exe} PRIVATE NXRT_TEST_GAME_REGISTER=${fn})
        endif()
    endforeach()
endif()
