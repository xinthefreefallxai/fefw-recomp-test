// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-service and host-facing entry points of the service library (S3c): what one service asks of another, and what the PC
// layers (window focus, settings UI) call to change console state. Implemented in the .cpp of the service named in each comment.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt::services {

// ---- fs (fs.cpp): save data backend used by am (EnsureSaveData ...) and the fsp-srv commands ---------------------------------------
// A user id is the 128-bit nn::account::Uid as two u64 halves in memory order: the first u64 (uid[0]) is "hi", the second is "lo" (profile().user_uid_hi / _lo).
// Results are Horizon result codes (0 = success; the module-2 fs results otherwise).
namespace fsapi {
// Creates the account save data of the title for `uid` when it does not exist (size and journal from the profile). *created tells which.
uint32_t ensure_account_save(uint64_t uid_hi, uint64_t uid_lo, bool* created);
// Size and journal size of the account save data (the values nn::fs::GetSaveDataSize reports); the fs "path not found" result when absent.
uint32_t account_save_size(uint64_t uid_hi, uint64_t uid_lo, int64_t* size, int64_t* journal);
}  // namespace fsapi

// ---- am (am.cpp): console state changes from the host --------------------------------------------------------------------------------
// Each call queues the matching AppletMessage for the guest (it sees them through ICommonStateGetter::ReceiveMessage, with the message
// event signalled) and updates the state the getters report.
namespace amctl {
void set_docked(bool docked);          // OperationModeChanged (30) + PerformanceModeChanged (31); GetOperationMode/GetPerformanceMode follow
void set_focus(bool in_focus);         // FocusStateChanged (15); GetCurrentFocusState: 1 = InFocus, 2 = OutOfFocus
bool docked();
bool in_focus();
}  // namespace amctl

// ---- the software keyboard library applet (am.cpp, swkbd_prompt.cpp) ---------------------------------------------------------------
// Where the keyboard's text comes from, in order: [applet] swkbd_text when set (test runs: NXRT_APPLET_SWKBD_TEXT); else the on-screen
// keyboard when enabled (drawn by the overlay inside the game window; the play layer enables it when there is an overlay); else the initial
// text the game gave; else "Player".
namespace swkbdui {
struct Request {
    std::string title;        // the window title (the game's name)
    std::string header, sub, guide, ok_text, initial;   // UTF-8, from the keyboard config and work buffer
    uint32_t max_len = 0;     // characters, 0 = no limit
    uint32_t min_len = 0;
    bool password = false;
};
void enable_prompt(bool on);
bool prompt_enabled();
// Posts the question and waits until the keyboard answers it. True with *text set on OK, false on cancel. Any thread but the one drawing.
bool ask(const Request& r, std::string* text);
// For the keyboard: the open question's id (0 = none), with *r set when there is one; answer() closes it (a stale id is ignored).
uint64_t pending(Request* r);
void answer(uint64_t id, bool ok, const std::string& text);
}  // namespace swkbdui

// ---- vi (vi_nv.cpp)----------------------------------------------------------------------------------------------------------------
namespace viapi {
// A new readable handle of the display's 60 Hz vsync event (starts the vsync thread on first use); 0 when no handle is left. For the
// Switch 2 nn::vi functions that answer on the host (core/switch2.cpp).
uint32_t display_vsync_handle();
}

// ---- code mods (mods_dll.cpp, nxrt/mod_api.h) ------------------------------------------------------------------------------------
namespace modapi {
// Loads the exefs/*.dll of the installed mods and calls their nxmod_init (once; later calls do nothing). base = main's guest address,
// frame_update = the game's frame boundary relative to base (GamePlay::frame_update; 0 = no frame calls). Play mode start, main thread.
void load_dll_mods(uint64_t base, uint64_t frame_update);
}  // namespace modapi

// ---- acc (acc.cpp) ----------------------------------------------------------------------------------------------------------------
namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo);   // the one local user (profile())
}

}  // namespace nxrt::services
