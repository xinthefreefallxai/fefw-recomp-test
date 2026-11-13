# SPDX-License-Identifier: GPL-3.0-or-later
# The Fire Emblem: Fortune's Weave game module (docs/GAME_BOUNDARY.md, fefw_game.h). Libraries, each a superset of the one before:
#   nxrt_game_fefw_data   identity, file names (pure data; the launcher links it)
#   nxrt_game_fefw_core   + title facts (links nxrt)
#   nxrt_game_fefw        + play-mode hooks (links nxrt_gfx; only with NXRT_BUILD_GFX). fefw.exe links this one.
set(_fefw "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")

add_library(nxrt_game_fefw_data STATIC "${_fefw}/fefw_data.cpp")
target_include_directories(nxrt_game_fefw_data PUBLIC "${_rt}/include" "${_rt}/games" "${_fefw}")
target_compile_features(nxrt_game_fefw_data PUBLIC cxx_std_20)

add_library(nxrt_game_fefw_core STATIC "${_fefw}/fefw_core.cpp")
target_link_libraries(nxrt_game_fefw_core PUBLIC nxrt_game_fefw_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_fefw STATIC "${_fefw}/fefw_game.cpp" "${_fefw}/fefw_play.cpp" "${_fefw}/fefw_res.cpp" "${_fefw}/fefw_twprobe.cpp" "${_fefw}/fefw_prompts.cpp"
                "${_fefw}/fefw_menu.cpp" "${_fefw}/fefw_wide.cpp" "${_fefw}/fefw_mouse.cpp" "${_fefw}/fefw_sysmenu.cpp")
    target_include_directories(nxrt_game_fefw PUBLIC "${_rt}/src/gfx" "${_rt}/src/gfx/play")
    target_link_libraries(nxrt_game_fefw PUBLIC nxrt_game_fefw_core nxrt_gfx)
endif()
