# SPDX-License-Identifier: GPL-3.0-or-later
# The Fire Emblem: Fortune's Weave game module (docs/GAME_BOUNDARY.md, ObeliskChamber_game.h). Libraries, each a superset of the one before:
#   nxrt_game_ObeliskChamber_data   identity, file names (pure data; the launcher links it)
#   nxrt_game_ObeliskChamber_core   + title facts (links nxrt)
#   nxrt_game_ObeliskChamber        + play-mode hooks (links nxrt_gfx; only with NXRT_BUILD_GFX). ObeliskChamber.exe links this one.
set(_oc "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")

add_library(nxrt_game_ObeliskChamber_data STATIC "${_oc}/ObeliskChamber_data.cpp")
target_include_directories(nxrt_game_ObeliskChamber_data PUBLIC "${_rt}/include" "${_rt}/games" "${_oc}")
target_compile_features(nxrt_game_ObeliskChamber_data PUBLIC cxx_std_20)

add_library(nxrt_game_ObeliskChamber_core STATIC "${_oc}/ObeliskChamber_core.cpp")
target_link_libraries(nxrt_game_ObeliskChamber_core PUBLIC nxrt_game_ObeliskChamber_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_ObeliskChamber STATIC "${_oc}/ObeliskChamber_game.cpp" "${_oc}/ObeliskChamber_play.cpp" "${_oc}/ObeliskChamber_res.cpp" "${_oc}/ObeliskChamber_twprobe.cpp" "${_oc}/ObeliskChamber_prompts.cpp"
                "${_oc}/ObeliskChamber_menu.cpp" "${_oc}/ObeliskChamber_wide.cpp" "${_oc}/ObeliskChamber_mouse.cpp" "${_oc}/ObeliskChamber_sysmenu.cpp" "${_oc}/ObeliskChamber_optmenu.cpp" "${_oc}/ObeliskChamber_saveslot.cpp" "${_oc}/ObeliskChamber_modmenu.cpp")
    target_include_directories(nxrt_game_ObeliskChamber PUBLIC "${_rt}/src/gfx" "${_rt}/src/gfx/play")
    target_link_libraries(nxrt_game_ObeliskChamber PUBLIC nxrt_game_ObeliskChamber_core nxrt_gfx)
endif()
