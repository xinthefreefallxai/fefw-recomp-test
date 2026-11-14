// SPDX-License-Identifier: GPL-3.0-or-later
// The Fire Emblem: Fortune's Weave game module (docs/GAME_BOUNDARY.md): what the game-independent base needs to run this title, as one
// nxrt::game::Info. Libraries (games/ObeliskChamber/CMakeLists.txt), each a superset of the one before:
//   nxrt_game_ObeliskChamber_data   identity, file names (pure data)        the launcher           nxrt_register_game_ObeliskChamber_data()
//   nxrt_game_ObeliskChamber_core   + title facts (needs nxrt)              the tests              nxrt_register_game_ObeliskChamber_core()
//   nxrt_game_ObeliskChamber        + play-mode hooks (needs nxrt_gfx)      ObeliskChamber.exe              nxrt_register_game_ObeliskChamber()
// Play mode: the 60 fps option and the game clock log (ObeliskChamber_play.cpp), the button glyphs (ObeliskChamber_prompts.cpp); no timestep or probes yet. The game exe
// (game-build/fefw/fefw_main.cpp) calls nxrt_register_game_ObeliskChamber() before nxrt::init().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "nxrt/game.h"

struct Ctx;   // nxrt/abi.h: the guest registers

void nxrt_register_game_ObeliskChamber();
void nxrt_register_game_ObeliskChamber_core();
void nxrt_register_game_ObeliskChamber_data();

namespace nxrt::play {
class GameHost;
class UiHost;
struct Settings;
}  // namespace nxrt::play
namespace nxrt::gfx {
class Overlay;
}

namespace nxrt::games::ObeliskChamber {
const game::Info& data_info();     // ObeliskChamber_data.cpp
const game::Info& core_info();     // ObeliskChamber_core.cpp
const game::Info& full_info();     // ObeliskChamber_game.cpp

// ObeliskChamber_play.cpp
void play_install(uint64_t base, play::GameHost* host, const play::Settings& s);
void play_remove();
// The game's clock against real time over the last few seconds (sampled once a second) and the frame-rate options read at start.
struct ClockInfo {
    bool valid = false;
    double rate = 0;          // 1.000 = the right speed
    int window_s = 0;         // seconds the rate covers
    int target = 0, display = 0;
    float step = 0;           // the last step, frames of 1/59.94 s
    int period_s = 0;         // [fefw] clock_log in force (0 = no log lines)
    bool fps60 = false, fps120 = false, smooth = false;
};
ClockInfo clock_info();
void set_clock_log(int period_s);   // live

// ObeliskChamber_menu.cpp: the F1 menu (GamePlay::settings_menu)
void settings_menu(play::UiHost& host, gfx::Overlay& overlay);

// ObeliskChamber_res.cpp: [fefw] output / render_ratio / render / shadow_size / quality (the render configs, notes/resolution.md)
void res_install(uint64_t base);
// Settings the Recomp Options screen changes while the game runs (main thread).
void res_live_apply(bool force);     // [fefw] render_ratio, shadow_size, quality were written; force: rebuild though the size is the same
void res_frame();                    // once per frame: the second half of a forced rebuild
bool res_dynres_live(bool on);       // [nvn] dynres; false: it cannot run (no [fefw] output)
void play_set_rate(int fps);         // 30, 60 or 120
void play_set_smooth(int mode);      // -1 auto, 0 off, 1 on
// The render-target pool's allocator was given room past the pool ([fefw] rt_pool_overflow_mb): renders above half of a 2160p output fit.
bool res_rt_pool_extended();

// ObeliskChamber_wide.cpp: outputs wider than 16:9 ([fefw] ultrawide; hooks after Kompas9's ultrawide patch). res_install calls wide_install with
// the output it gives the game; nothing is installed unless that output is wider than 16:9.
std::string wide_mode();                             // [fefw] ultrawide: "auto" or "off"
bool wide_aspect(int w, int h);                      // wider than 16:9 (by more than 1 %)
void wide_install(uint64_t base, int out_w, int out_h);
void wide_remove();
struct WideInfo {
    bool active = false;
    int out_w = 0, out_h = 0;
    float k = 1.0f;                                  // the output's width over a 16:9 picture's of the same height
    static constexpr int kCounters = 10;
    uint64_t counts[kCounters] = {};                 // times each hook did its work
    const char* names[kCounters] = {};
};
WideInfo wide_info();

// ObeliskChamber_mouse.cpp: the game's own Joy-Con 2 mouse controls, read for the log, the tests and the F1 menu
struct MouseInfo {
    bool valid = false;
    bool mode = false;                 // the game is in its mouse controls
    bool manual = false;               // turned on with SL + SR (the manual switching setting)
    bool allowed = false, enabled = false;
    int32_t dx = 0, dy = 0;            // the game's movement this frame (pointer units)
    int32_t cursor_x = 0, cursor_y = 0;   // the game's pointer, 1920 x 1080
    float cfg[5] = {};                 // +0xc8..+0xd8: seconds to switch on, movement threshold (squared), two motion thresholds, seconds to switch off
    uint64_t frames = 0, frames_mouse = 0, entries = 0, exits = 0;
};
void mouse_install(uint64_t base);
void mouse_frame();                    // main thread, once per frame
MouseInfo mouse_info();

// ObeliskChamber_sysmenu.cpp: a "Recomp Options" entry under System in the game's + menu that opens the F1 menu ([fefw] system_menu_entry)
void sysmenu_install(uint64_t base);
void sysmenu_frame();                  // main thread, once per frame
// ObeliskChamber_optmenu.cpp: the recomp's settings as a screen of the game's own Options class, opened by that entry ([fefw] native_options)
void optmenu_install(uint64_t base);
void optmenu_frame();                  // main thread, once per frame
bool optmenu_open(::Ctx* c, uint64_t item);   // from the entry's ACall; false: not available (the F1 menu is used)
// A pick list on that screen class: up to three tabs (L / R), `count` rows, each on one tab with a label and a describing value, a help
// line, and pick(c, row) on A. pick answers what the list does next: 3 closes it, 12 leaves it waiting under a dialog the pick opened,
// 13 keeps it open and redraws the row's value, 0 does nothing more. The callbacks run on the game's main thread and write UTF-8 into `out`.
struct OptList {
    int pages = 1;
    const char* titles[3] = {};        // the tabs' names (under 23 bytes each)
    int count = 0;
    int (*page)(int row) = nullptr;    // the row's tab; nullptr: all on the first
    void (*label)(int row, char* out, size_t n) = nullptr;
    void (*value)(int row, char* out, size_t n) = nullptr;
    void (*help)(int row, char* out, size_t n) = nullptr;
    uint32_t (*pick)(::Ctx* c, int row) = nullptr;   // on the game thread, inside the row's A handler
    void (*back)() = nullptr;          // B on a row: the list is closing
    bool (*dirty)() = nullptr;         // true while the rows' values may have changed: they are drawn again (a few times a second)
    uint64_t fn[5] = {};               // internal: the tab label function object
};
bool optmenu_open_list(::Ctx* c, uint64_t item, const OptList& list);
// ObeliskChamber_saveslot.cpp: several copies of the game's save folder ("000", "001", ...) chosen from the game's own Continue, New Game, Delete Save
// and Save entries ([fefw] save_slots, [fefw] save_slot)
void saveslot_install(uint64_t base);
void saveslot_frame();                 // main thread, once per frame
// ObeliskChamber_modmenu.cpp: the title's DLC entry reads "DLC / Mods" and opens a list of the mods folder with an on / off switch per mod
// ([fefw] mod_menu)
void modmenu_install(uint64_t base);
void modmenu_frame();                  // main thread, once per frame

// ObeliskChamber_prompts.cpp: the button glyphs of the game's text follow the controller in use ([prompts] style; notes/prompts.md)
void prompts_install(play::GameHost* host);
void prompts_remove();
void prompts_frame();      // main thread, once per frame (GamePlay::frame)
}  // namespace nxrt::games::ObeliskChamber
