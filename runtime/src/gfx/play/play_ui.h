// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings page (the "Controls" window of the F1 overlay). The page only talks to the game through UiHost, so it can be
// tested and developed without a game running. It runs inside ImGui on the presenting thread; UiHost implementations are thread safe.
#pragma once
#include <cstdint>
#include <string>

#include "play_engine.h"
#include "play_types.h"

namespace nxrt::play {

struct LiveStatus {
    Ctx ctx = Ctx::Menu;               // the context the engine is in now
    const char* aim = "";              // why the mouse aim is on or off ("aim on", "locked on", "not in game" ...)
    bool mouse_captured = false;       // the pointer is captured (relative mode)
    int pads = 0;                      // open gamepads
    std::string pad_name;              // the first one
    bool pad_gyro = false, pad_rumble = false;
    const char* prompt_now = "";       // the button prompt style shown right now ("xbox" when Auto follows an Xbox pad); the Controller tab shows it under Auto
    double fps = 0;                    // presented frames per second (measured over the last second)
    const char* present_mode = "";     // "fifo" "mailbox" "immediate" ...
    const char* window_mode = "";      // "windowed" "borderless" "exclusive" (what the window is, not what the settings ask)
    float refresh_hz = 0;              // refresh rate of the display the window is on
    int width = 0, height = 0;         // window size in pixels
    EngineOut out;                     // what the keyboard and mouse press on the Switch pad right now
    double pitch_deg = 0;              // the aim's pitch
    uint64_t input_calls = 0;          // the game's per-tick player-input calls seen (GameHost::input_tick; 0: the game has not reached gameplay)
};

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual Settings settings() const = 0;               // the settings in force (a copy)
    virtual Settings factory() const = 0;                // the factory settings
    // Queues new settings: the game's main thread applies them at its next frame (bindings, sensitivity, window mode, vsync, frame cap)
    // and writes them to the settings file. Cheap to call; the page calls it when the user changes something (not every frame).
    virtual void apply(const Settings& s) = 0;
    virtual LiveStatus status() const = 0;
    virtual std::string settings_path() const = 0;
    // Rebinding: after begin_capture() the next key press, mouse button or wheel notch does not reach the game or the page; it is returned
    // by capture_result() instead. Escape cancels (capturing() turns false, no result).
    virtual void begin_capture() = 0;
    virtual bool capturing() const = 0;
    virtual bool capture_result(InputId* out) = 0;       // true once per captured input
    virtual void cancel_capture() = 0;
    // An input the page must not let the player bind (the key that opens this menu). The page keeps waiting for another one.
    virtual bool reserved_key(const InputId& id) const { (void)id; return false; }
};

// Draws the settings menu: pages Display, Graphics, Controls (mouse, keyboard bindings, gamepad, Switch layout, status), Audio and About, plus
// a Game page when the registered game has tweaks of its own. Call between ImGui::Begin / End of the window. Everything is laid out in units
// of the font size, so it follows the overlay's UI scale (overlay.cpp). Keeps its editing state in a function-local static (one page per process).
void draw_settings_page(UiHost& host);

// The About page (play_ui_about.cpp): version, build and the folders and files in use. Draws into the current window.
void draw_about_page(const std::string& settings_path);

// The settings host outside the menu's draw call (a game module's own settings screen). Its settings() and apply() take the host's lock:
// any thread. Never null.
UiHost* ui_host();

}  // namespace nxrt::play
