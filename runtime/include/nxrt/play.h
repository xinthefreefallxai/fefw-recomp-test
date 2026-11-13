// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (S8): the PC front end of the recompiled game. One window, the SDL event pump on the game's main thread, the frame
// limiter on the presenting thread, real controller / keyboard / mouse input into the hid service, raw mouse aim, and the F1 settings page.
//
// Start-up order in a launcher (game-build/src/mpr_main.cpp):
//   nxrt::init();  play::load();                     // reads controls.ini (or [play] controls) and the [play] keys of nxrt.ini
//   window + gfx::Device + gfx::Renderer             // window size and present mode come from play::display()
//   nvn::install_backend({... limiter = play::limiter(), keep_aspect = play::display().keep_aspect})
//   play::start({...});                              // input source, hooks, overlay page; the game may run from here
// What is about one game (the frame-boundary site, raw mouse aim, camera look, lock-on, display hooks, menu widgets, context detection, F1
// entries) comes from the registered game module's play table (nxrt/game.h, src/gfx/play/play_game.h; docs/GAME_BOUNDARY.md). The game must
// have been lifted with entry hooks at those sites (Metroid Prime Remastered: recomp/games/mpr/main.sites). Without them the window still
// opens but nothing is pumped (a warning says so after a few seconds).
//
// Configuration (nxrt.ini section [play], environment NXRT_PLAY_<KEY>):
//   controls = controls.ini     the settings file (play_types.h documents the format); written with the factory settings when missing
//   capture_mouse = 1           capture the pointer (relative mode) while the game is being played
//   pitch_limit_deg = 0         > 0: pitch limit of the mouse aim in degrees instead of the game's tweak value
//   quit_on_close = 1           closing the window ends the process (exit code 0)
//   game_base = 0x7100000000    base of the game's main module
//   log_status = 0              1: one status line per second in the log (context, aim state, input counts)
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"

namespace nxrt::play {

// Loads the settings file and creates the frame limiter. Idempotent. Returns false (and uses the factory settings) when the file is invalid.
bool load();
// What the launcher needs from the settings before the window exists.
struct DisplayConfig {
    int width = 1280, height = 720;         // windowed client size
    std::string window_mode = "windowed";   // "windowed" "borderless" "exclusive"
    bool vsync = false;
    int fps_cap = 60;                       // 0 = unlimited
    bool keep_aspect = true;
    gfx::PresentMode present_mode = gfx::PresentMode::Immediate;   // what the vsync setting asks for (the renderer falls back to what the surface supports)
};
DisplayConfig display();
FrameLimiter* limiter();                    // owned by play mode; pass to nvn::BackendDesc::limiter
std::string settings_path();

struct StartOptions {
    gfx::Device* device = nullptr;          // needed for the overlay
    gfx::Renderer* renderer = nullptr;      // needed for the overlay and for present-mode changes; null = no graphics (unit tests)
    bool window = false;                    // a platform window exists: pump its events, capture the pointer, fullscreen, quit on close
    bool overlay = true;                    // create the F1 overlay (needs device and renderer)
};
bool start(const StartOptions& opt);        // after nxrt::init, the renderer and the NVN back end; installs the hid input source and the hooks
void stop();                                // saves changed settings, removes the hooks and the input source

// Counters for the tests and the log.
struct Stats {
    uint64_t frames = 0;                    // Update hook calls
    uint64_t process_input_calls = 0;
    uint64_t aim_frames = 0;                // ProcessInput calls where the mouse aim was applied
    uint64_t events = 0;                    // input events drained
    uint64_t mouse_counts_applied = 0;      // sum of |dx| + |dy| taken by the aim
    bool pump_thread_mismatch = false;      // the Update hook ran on another thread than start() was called on
};
Stats stats();
// Shows the F1 settings menu (the game thread that feeds input; nothing when there is no overlay). The pad drives it while it is up.
void open_settings_menu();

}  // namespace nxrt::play
