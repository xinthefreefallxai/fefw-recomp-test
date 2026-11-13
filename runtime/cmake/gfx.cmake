# SPDX-License-Identifier: GPL-3.0-or-later
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt) and the test executable nxrt_gfx_tests (CTest groups gfx_*).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of the tools repository is compiled in as source (tools/tiling, tegra_tiling.h): found at
# ../tools or, from a worktree under runtime-wt/<name>, at ../../tools.
set(NXRT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools" CACHE PATH "The tools repository (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tiling/src/tegra_tiling.cpp")
    get_filename_component(_nxrt_td "${CMAKE_CURRENT_SOURCE_DIR}/../../tools" ABSOLUTE)
    set(NXRT_TOOLS_DIR "${_nxrt_td}" CACHE PATH "The tools repository (tools/tiling is used)" FORCE)
endif()
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

# ASTC -> BC7 transcoder (tools/textures): the prebuilt static libraries of its ISPC build (scripts/build.ps1 -Ispc there)
# and the decoder DLLs that astc2bc7 loads at run time (copied next to every executable that links nxrt_gfx). Without that build it is
# compiled here from source: bc7e translated to C++ and the astcenc decoder linked in (tools/textures/cmake/a2b_cpp.cmake; no ISPC, no DLLs).
# NXRT_ASTC2BC7=OFF builds without it (src/gfx/nvn/nvn_a2b_off.cpp): for a machine without tools/textures/third_party; ASTC textures then fail.
option(NXRT_ASTC2BC7 "Link the ASTC -> BC7 transcoder of tools/textures (OFF: ASTC textures are not prepared)" ON)
set(NXRT_A2B_DIR "${NXRT_TOOLS_DIR}/textures/build/release" CACHE PATH "Build folder of tools/textures (astc2bc7.lib, bc7e_ispc.lib, astcdec-*.dll)")
set(NXRT_A2B_DLLS "")
if(NXRT_ASTC2BC7)
    include("${NXRT_TOOLS_DIR}/textures/cmake/a2b_cpp.cmake" OPTIONAL)
    set(_nxrt_a2b_src FALSE)
    if(COMMAND a2b_cpp_sources_present)
        a2b_cpp_sources_present(_nxrt_a2b_src)
    endif()
    if(EXISTS "${NXRT_A2B_DIR}/astc2bc7.lib" AND EXISTS "${NXRT_A2B_DIR}/bc7e_ispc.lib")
        add_library(nxrt_astc2bc7 INTERFACE)
        target_include_directories(nxrt_astc2bc7 INTERFACE "${NXRT_TOOLS_DIR}/textures/include")
        target_link_libraries(nxrt_astc2bc7 INTERFACE "${NXRT_A2B_DIR}/astc2bc7.lib" "${NXRT_A2B_DIR}/bc7e_ispc.lib")
        file(GLOB NXRT_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
    elseif(_nxrt_a2b_src)
        message(STATUS "nxrt: ASTC transcoder built from source (bc7e in C++; no ISPC build in ${NXRT_A2B_DIR})")
        a2b_add_cpp_library(nxrt_astc2bc7)
    else()
        message(FATAL_ERROR "no ASTC transcoder: neither the ISPC build of tools/textures in ${NXRT_A2B_DIR} nor its sources (tools/textures/scripts/fetch_third_party.ps1); set NXRT_A2B_DIR, or configure with -DNXRT_ASTC2BC7=OFF")
    endif()
else()
    message(STATUS "nxrt: NXRT_ASTC2BC7=OFF: no ASTC transcoder (ASTC textures are not prepared)")
    add_library(nxrt_astc2bc7 STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/nvn/nvn_a2b_off.cpp")
    target_include_directories(nxrt_astc2bc7 PUBLIC "${NXRT_TOOLS_DIR}/textures/include")
endif()
# also for a project that adds the runtime as a subdirectory and copies the DLLs next to its own exe (game-build/fefw)
set(NXRT_A2B_DLLS "${NXRT_A2B_DLLS}" CACHE INTERNAL "astcdec DLLs of the prebuilt ISPC transcoder, copied next to the executables")

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "nvn_gpu_stub[.]cpp$")   # unit-test stand-in of the GPU boundary (scripts/nvn_unit.ps1)
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "nvn_a2b_off[.]cpp$")    # the transcoder's stand-in (NXRT_ASTC2BC7=OFF above)
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
# The About page of the F1 menu shows the commit this build was made from (read when CMake configures; absent without git).
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE NXRT_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _nxrt_git_rc)
    if(_nxrt_git_rc EQUAL 0 AND NXRT_BUILD_COMMIT)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/play/play_ui_about.cpp" PROPERTIES COMPILE_DEFINITIONS "NXRT_BUILD_COMMIT=\"${NXRT_BUILD_COMMIT}\"")
    endif()
endif()
target_include_directories(nxrt_gfx PUBLIC include src/gfx/nvn/third_party PRIVATE src/gfx src/gfx/nvn)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 nxrt_zstd SDL3::SDL3-static)
# BC1 encoder for [gfx] astc_bc = bc1 (src/gfx/nvn/nvn_bc1.cpp): rgbcx of bc7enc_rdo (MIT), fetched with the transcoder's sources. Without it the
# setting falls back to off.
set(_nxrt_rgbcx "${NXRT_TOOLS_DIR}/textures/third_party/bc7enc_rdo")
if(NXRT_ASTC2BC7 AND EXISTS "${_nxrt_rgbcx}/rgbcx.cpp" AND EXISTS "${_nxrt_rgbcx}/rgbcx.h")
    target_sources(nxrt_gfx PRIVATE "${_nxrt_rgbcx}/rgbcx.cpp")
    set_source_files_properties("${_nxrt_rgbcx}/rgbcx.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    set_source_files_properties("${_nxrt_rgbcx}/rgbcx.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/nvn/nvn_bc1.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_nxrt_rgbcx}")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/nvn/nvn_bc1.cpp" PROPERTIES COMPILE_DEFINITIONS NXRT_HAVE_RGBCX=1)
endif()
# The core calls into the graphics shell (src/core/switch2.cpp: the HDR layer luminance; src/testtools: the screenshot wait), so whatever
# links nxrt (nxrt_tests, the liftrun_* tests) links nxrt_gfx too, as the game exe does. Static libraries may depend on each other.
target_link_libraries(nxrt INTERFACE nxrt_gfx)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

add_executable(nxrt_gfx_demo apps/gfx_demo.cpp)
target_link_libraries(nxrt_gfx_demo PRIVATE nxrt_gfx)
target_link_options(nxrt_gfx_demo PRIVATE /MAP /DEBUG /STACK:8388608)
foreach(dll ${NXRT_A2B_DLLS})
    add_custom_command(TARGET nxrt_gfx_demo POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_demo>")
endforeach()

if(NXRT_BUILD_TESTS)
    file(GLOB_RECURSE NXRT_GFX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/*.cpp")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/mpr")   # the prompt icon test reads Metroid's paks through games/mpr's pak reader
        list(FILTER NXRT_GFX_TEST_SOURCES EXCLUDE REGEX "test_prompts[.]cpp$")
    endif()
    # The tests of the registered test game's own code (games/<id>/tests) run in the same executable (docs/GAME_BOUNDARY.md).
    if(NXRT_TEST_GAME)
        file(GLOB NXRT_GAME_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${NXRT_TEST_GAME}/tests/*.cpp")
        list(APPEND NXRT_GFX_TEST_SOURCES ${NXRT_GAME_TEST_SOURCES})
    endif()
    add_executable(nxrt_gfx_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp" ${NXRT_GFX_TEST_SOURCES})
    target_include_directories(nxrt_gfx_tests PRIVATE tests src/gfx src/gfx/nvn)
    target_link_libraries(nxrt_gfx_tests PRIVATE nxrt_gfx)
    target_compile_definitions(nxrt_gfx_tests PRIVATE NXRT_GFX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gfx")
    # The BC7 reference decoder of bc7enc_rdo (public domain; the same file the setup tool checks its texture cache with): the ASTC texture scene
    # works out its expected picture from the current encoder instead of a stored image (tests/gfx/test_nvn_scene_texture.cpp).
    set(NXRT_BC7_REF_DIR "${NXRT_TOOLS_DIR}/textures/third_party/bc7enc_rdo")
    if(EXISTS "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        target_sources(nxrt_gfx_tests PRIVATE "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        target_include_directories(nxrt_gfx_tests PRIVATE "${NXRT_BC7_REF_DIR}")
        target_compile_definitions(nxrt_gfx_tests PRIVATE NXRT_HAVE_BC7_REF=1)
    endif()
    target_link_options(nxrt_gfx_tests PRIVATE /MAP /STACK:8388608 /DEBUG)
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_gfx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_tests>")
    endforeach()

    set(NXRT_GFX_GROUPS "")
    foreach(src ${NXRT_GFX_TEST_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP[(]")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP[(]([a-z0-9_]+)[)].*$" "\\1" g "${l}")
            list(APPEND NXRT_GFX_GROUPS ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES NXRT_GFX_GROUPS)
    foreach(g ${NXRT_GFX_GROUPS})
        add_test(NAME ${g} COMMAND nxrt_gfx_tests ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT 300 ENVIRONMENT "NXRT_GFX_HIDDEN=1")
    endforeach()
    # Timing oracles must not share the machine with another test of this suite.
    foreach(g gfx_limiter gfx_pacing)
        if(TEST ${g})
            set_tests_properties(${g} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
    if(TEST gfx_pacing)
        set_tests_properties(gfx_pacing PROPERTIES TIMEOUT 900)   # up to four attempts per case on a busy GPU
    endif()
    message(STATUS "nxrt gfx test groups: ${NXRT_GFX_GROUPS}")
endif()

# The launcher (apps/launcher).
include(${CMAKE_CURRENT_LIST_DIR}/launcher.cmake)
