# SPDX-License-Identifier: GPL-3.0-or-later
# fefw.exe: the runtime (../../runtime) + the six lifted game libraries (built by ../../recomp/scripts/build_game.cmd fefw into ../../work/build/fefw).
#   pwsh game-build\scripts\build_fefw.ps1          configure + build into game-build\fefw\build\release
# Nothing derived from the game lives here: the libraries are imported from the work folder; the NSOs and the RomFS are read at run time
# where the dump is (the defaults below are baked into fefw_paths.h; [fs] exefs / main / romfs in fefw.ini override them).
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT), as the runtime and the lifted libraries are built.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(fefw_game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(FEFW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FEFW_LIB_DIR "${FEFW_ROOT}/work/build/fefw" CACHE PATH "folder with fefw_{main,sdk,subsdk0,subsdk1,subsdk2,rtld}.lib (recomp scripts build_game.cmd fefw)")
# Defaults only: fefw.ini ([fs] exefs / main / romfs, written by the launcher) says where the dump is. game/dump/exefs is the copy
# tools/nso/prepare_exefs.py makes from the dump (main uncompressed), so [fs] main can stay empty.
set(FEFW_EXEFS_DEFAULT "game/dump/exefs" CACHE PATH "default [fs] exefs")
set(FEFW_ROMFS_DEFAULT "game/dump/romfs" CACHE PATH "default [fs] romfs")
set(FEFW_MAIN_DEFAULT "" CACHE FILEPATH "default [fs] main: main as an uncompressed NSO (empty: <exefs>/main)")
configure_file(src/fefw_paths.h.in "${CMAKE_CURRENT_BINARY_DIR}/fefw_paths.h" @ONLY)

add_compile_options("-clang:-fmacro-prefix-map=${FEFW_ROOT}=.")
set(FEFW_RUNTIME_DIR "${FEFW_ROOT}/runtime" CACHE PATH "runtime source tree compiled into fefw.exe")
set(NXRT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NXRT_GAMES "ObeliskChamber" CACHE STRING "" FORCE)  # runtime/games/ObeliskChamber only
set(NXRT_RECOMP_INCLUDE "${FEFW_ROOT}/recomp/include" CACHE PATH "")
# The ASTC transcoder: the ISPC build of tools/textures when it exists, else built from its sources (cmake/gfx.cmake). Only without
# either is the runtime built with its stand-in. An OFF that an earlier configure cached for want of the ISPC build is dropped once.
if(DEFINED CACHE{NXRT_ASTC2BC7})
    get_property(_fefw_a2b_doc CACHE NXRT_ASTC2BC7 PROPERTY HELPSTRING)
    if(_fefw_a2b_doc STREQUAL "Link the ASTC -> BC7 transcoder of tools/textures")
        unset(NXRT_ASTC2BC7 CACHE)
    endif()
endif()
if(NOT DEFINED NXRT_ASTC2BC7 AND NOT EXISTS "${FEFW_ROOT}/tools/textures/build/release/astc2bc7.lib" AND
   NOT EXISTS "${FEFW_ROOT}/tools/textures/third_party/astc-encoder/Source/astcenc.h")
    set(NXRT_ASTC2BC7 OFF CACHE BOOL "Link the ASTC -> BC7 transcoder of tools/textures (OFF: no ISPC build and no sources)")
endif()
# Extra compiler flags for the runtime, its third-party code and fefw_main.cpp (not the lifted libraries: NX_EXTRA_FLAGS is theirs).
# build_fefw.ps1 passes the environment variable NX_RT_FLAGS on every configure. For a profile-guided, machine-specific runtime
# (docs/PGO.md, "The runtime"): -fprofile-instr-generate, then -fprofile-instr-use=<file> /clang:-march=native.
set(FEFW_RT_FLAGS "" CACHE STRING "extra compiler flags for the runtime and fefw_main.cpp")
if(FEFW_RT_FLAGS)
    separate_arguments(_fefw_rt_flags NATIVE_COMMAND "${FEFW_RT_FLAGS}")
    add_compile_options(${_fefw_rt_flags})
    if(FEFW_RT_FLAGS MATCHES "profile-instr-use")
        add_compile_options(-Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    endif()
    message(STATUS "fefw: runtime flags ${FEFW_RT_FLAGS}")
endif()
add_subdirectory("${FEFW_RUNTIME_DIR}" runtime)

# Release package (FEFW_RT_DLL=ON): no game code in anything built here. The runtime and the boot code become fefw_rt.dll (+ its import
# library fefw_rt.lib); fefw.exe is linked on the player's PC from the libraries lifted out of their own dump and src/fefw_stub.cpp
# (tools/build/fefw_build.py in the package, with the bundled Zig). Nothing lifted is imported here.
# Declared here, before the FEFW_RT_DLL return below: build_fefw.ps1 passes both on every configure (used at "ThinLTO" further down).
option(FEFW_THINLTO "the lifted libraries hold ThinLTO bitcode (-flto=thin): link options for lld-link's ThinLTO" OFF)
set(FEFW_LTO_JOBS "" CACHE STRING "ThinLTO back-end jobs of the fefw.exe link (empty: lld-link's default, one per physical core)")
option(FEFW_RT_DLL "build the runtime as fefw_rt.dll for the release package" OFF)
if(FEFW_RT_DLL AND NOT NXRT_ASTC2BC7)
    message(FATAL_ERROR "FEFW_RT_DLL=ON needs the ASTC transcoder (NXRT_ASTC2BC7=ON): without it desktop GPUs render most textures black")
endif()
if(FEFW_RT_DLL)
    add_library(fefw_rt SHARED src/fefw_main.cpp src/fefw_rt.def)
    target_include_directories(fefw_rt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" src)
    target_compile_definitions(fefw_rt PRIVATE FEFW_RT_DLL=1)
    target_link_libraries(fefw_rt PRIVATE nxrt_game_ObeliskChamber nxrt_gfx nxrt user32)
    # no .pdb path in the DLL; low host allocations as for fefw.exe (the guest ranges at 0x71_0000_0000 stay free)
    target_link_options(fefw_rt PRIVATE /HIGHENTROPYVA:NO /DEBUG:NONE /Brepro)
    target_compile_options(fefw_rt PRIVATE "-clang:-ffile-prefix-map=${FEFW_ROOT}=.")
    add_custom_command(TARGET fefw_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FEFW_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:fefw_rt>/fonts/DejaVuSans.ttf")
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET fefw_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:fefw_rt>")
    endforeach()
    return()
endif()

set(FEFW_LIBS "")
foreach(m main sdk subsdk0 subsdk1 subsdk2 rtld)
    add_library(fefw_${m} STATIC IMPORTED GLOBAL)
    set_target_properties(fefw_${m} PROPERTIES IMPORTED_LOCATION "${FEFW_LIB_DIR}/fefw_${m}.lib")
    list(APPEND FEFW_LIBS fefw_${m})
endforeach()

add_executable(fefw src/fefw_main.cpp res/fefw.rc)
set_source_files_properties(res/fefw.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/fefw.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/fefw.ico")
target_include_directories(fefw PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(fefw PRIVATE ${FEFW_LIBS} nxrt_game_ObeliskChamber nxrt_gfx nxrt)
# As pkp.exe: a linker map next to the exe (host frames get names), a big main stack (lifted code nests deeply), low host allocations so
# the guest ranges at 0x71_0000_0000 and up stay free. res/fefw.rc carries the manifest (UTF-8 code page).
target_link_options(fefw PRIVATE /MAP /STACK:134217728 /HIGHENTROPYVA:NO /DEBUG /MANIFEST:NO)
# ThinLTO (docs/PGO.md, "ThinLTO"): lifted libraries compiled with -flto=thin (in NX_EXTRA_FLAGS) hold LLVM bitcode, and lld-link optimises
# and inlines across all of them (and across the shards of each module) when it links fefw.exe. build_fefw.ps1 sets FEFW_THINLTO from
# NX_EXTRA_FLAGS on every configure. The cache makes a re-link after a small change fast; FEFW_LTO_JOBS limits the parallel back ends
# (empty: one per physical core), which bounds the link's memory.
if(FEFW_THINLTO)
    if(NOT CMAKE_LINKER MATCHES "lld-link")
        message(FATAL_ERROR "FEFW_THINLTO needs lld-link as the linker (CMAKE_LINKER is ${CMAKE_LINKER}); link.exe cannot read LLVM bitcode")
    endif()
    target_link_options(fefw PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
    if(FEFW_LTO_JOBS)
        target_link_options(fefw PRIVATE "/opt:lldltojobs=${FEFW_LTO_JOBS}")
    endif()
    message(STATUS "fefw: ThinLTO link (cache ${CMAKE_BINARY_DIR}/lto-cache, jobs ${FEFW_LTO_JOBS})")
endif()
# Profile-guided builds (docs/PGO.md): lifted libraries built with -fprofile-instr-generate name LLVM's profile runtime (clang_rt.profile*.lib) as a
# default library; it lives in clang's resource folder next to the compiler. Harmless for other builds (nothing there is named otherwise).
get_filename_component(_fefw_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(GLOB _fefw_clang_rt_dirs LIST_DIRECTORIES true "${_fefw_llvm_bin}/../lib/clang/*/lib/windows" "${_fefw_llvm_bin}/../lib/clang/*/lib/x86_64-pc-windows-msvc")
foreach(d ${_fefw_clang_rt_dirs})
    if(IS_DIRECTORY "${d}")
        target_link_directories(fefw PRIVATE "${d}")
    endif()
endforeach()

# The shared-font fallback (runtime src/services/pl.cpp looks in fonts\ next to the exe) and the ASTC decoder DLLs when the transcoder is in.
add_custom_command(TARGET fefw POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FEFW_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:fefw>/fonts/DejaVuSans.ttf")
foreach(dll ${NXRT_A2B_DLLS})
    add_custom_command(TARGET fefw POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:fefw>")
endforeach()
