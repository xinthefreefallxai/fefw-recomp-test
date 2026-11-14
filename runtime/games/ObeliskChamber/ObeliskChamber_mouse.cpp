// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the game's own Joy-Con 2 mouse controls (ObeliskChamber_game.h mouse_info, mouse_frame). The game runs its mouse mode
// from the Nmouse state hid gives it (hid.cpp sample_nmouse, docs/hid_mouse_sw2.md "Fortune's Weave"); this file reads it for the log, the
// tests and the F1 menu, makes the mode follow the device in use at once ([fefw] mouse_switch_now, handover below) and lines the game's
// pointer up with the Windows pointer when the mouse changes hands.
//
// The game's input object ([GOT 0x216a4b0]) holds the mouse manager at +0x2c8, updated every frame by 0x19c6ea0:
//   +0x00 / +0x60   the Joy-Con 2 (R) / (L) mouse records: the NmouseState (0x30), then +0x30 buttons held, +0x34 pressed, +0x38 released,
//                   +0x3c / +0x40 this frame's movement (x - 960, y - 540: the game puts the pointer back at the centre every frame),
//                   +0x44 / +0x48 wheel sums, +0x4c frames since the attribute cleared, +0x50 frames on the surface, +0x5c mouse mode by
//                   SL + SR (the manual setting), +0x5d mouse mode allowed (the switching setting), +0x5e in mouse mode (auto), +0x5f seen
//   +0xc8           frames on the surface before mouse mode starts (30; counted only while the Joy-Con lies flat), +0xcc: a movement whose
//                   square is larger starts it at once (12000)
//   +0xd0 / +0xd4 / +0xd8  the Joy-Con's own motion thresholds and the frames after the attribute clears before mouse mode ends (120)
//   +0xdc / +0xe0   the game's pointer in 1920 x 1080 (the right mouse's movement, kept on the screen)
//   +0xe4           mouse controls enabled
// The game asks "mouse mode?" through 0x19c7210: seen and (SL + SR, or enabled and allowed and in mouse mode), for either record.
// The settings (System > Settings: "Method for Switching to Mouse", "Menu Cursor Speed (Mouse)", "Map Cursor Speed (Mouse)", the two
// "Camera Sensitivity (Mouse)") are read by 0x1584e.. / 0x1585020 into the mouse system [0x22983a8] and the flags above.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "ObeliskChamber_game.h"
#include "nxrt/config.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/tt.h"

namespace nxrt::games::ObeliskChamber {

namespace {

constexpr const char* kTag = "fefw";
constexpr uint64_t kInputGot = 0x216a4b0;   // GOT slot: the address of the input object's pointer
constexpr uint64_t kMouseMgr = 0x2c8;

uint64_t g_base = 0;
uint64_t g_frames_mouse = 0, g_frames = 0, g_entries = 0, g_exits = 0, g_forced_on = 0, g_forced_off = 0;
uint64_t g_lifts_seen = 0;
bool g_was = false, g_cfg_logged = false;
bool g_was_engaged = false;
bool g_switch_now = true;      // [fefw] mouse_switch_now

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}

uint64_t mouse_mgr() {
    uint64_t var = 0, input = 0;
    if (!g_base || !rd(g_base + kInputGot, &var) || !rd(var, &input) || !input) return 0;
    return mem::is_mapped(input + kMouseMgr, 0x100) ? input + kMouseMgr : 0;
}

bool record_mode(const uint8_t* m, int rec) {
    const uint8_t* r = m + rec * 0x60;
    return r[0x5f] == 1 && ((r[0x5c] & 1) || (m[0xe4] == 1 && (r[0x5d] & 1) && (r[0x5e] & 1)));
}

}  // namespace

void mouse_install(uint64_t base) {
    g_base = base;
    g_switch_now = config_get_bool("fefw", "mouse_switch_now", true);
}

MouseInfo mouse_info() {
    MouseInfo mi;
    const uint64_t mgr = mouse_mgr();
    if (!mgr) return mi;
    uint8_t m[0x100];
    std::memcpy(m, reinterpret_cast<const void*>(mgr), sizeof m);
    mi.valid = true;
    mi.mode = record_mode(m, 0) || record_mode(m, 1);
    mi.manual = (m[0x5c] & 1) || (m[0x60 + 0x5c] & 1);
    mi.allowed = (m[0x5d] & 1) != 0;
    mi.enabled = m[0xe4] == 1;
    std::memcpy(&mi.dx, m + 0x3c, 4);
    std::memcpy(&mi.dy, m + 0x40, 4);
    std::memcpy(&mi.cursor_x, m + 0xdc, 4);
    std::memcpy(&mi.cursor_y, m + 0xe0, 4);
    std::memcpy(mi.cfg, m + 0xc8, sizeof mi.cfg);
    mi.frames = g_frames;
    mi.frames_mouse = g_frames_mouse;
    mi.entries = g_entries;
    mi.exits = g_exits;
    return mi;
}

// The game's own switch is made for a Joy-Con lying on a table: mouse mode starts after +0xc8 frames (30) with the attribute set, and ends
// +0xd8 frames (120, 2 s) after it clears unless the Joy-Con's six-axis says it was picked up. On a PC the hid layer already decides who
// has control (the last device used: hid.cpp sample_nmouse), so with the game's switching method on Automatic (+0x5d) the wait is cut out:
// the mouse put down by real use starts mouse mode at once (+0x50 = +0xc8, the game's own "long enough" test passes this frame), and a key
// or the pad ends it at once (+0x5e = 0, as the game's own timeout would). The manual method (SL + SR, +0x5c) is left alone. Main thread,
// between the game's frames (the present wrapper), so the game's update (0x19c6ea0, same thread) never sees a half write.
void handover(uint64_t mgr, const MouseInfo& mi) {
    const bool engaged = hid::jc2_mouse_engaged();
    const uint64_t lifts = hid::jc2_mouse_stats().lifts;
    const bool lifted_now = lifts != g_lifts_seen;
    g_lifts_seen = lifts;
    if (!mi.enabled) {
        g_was_engaged = engaged;
        return;
    }
    for (int rec = 0; rec < 2; ++rec) {
        uint8_t* r = reinterpret_cast<uint8_t*>(mgr + rec * 0x60);
        if (!(r[0x5d] & 1) || (r[0x5c] & 1)) continue;          // switching method manual (or SL + SR mode): the game's own rules
        if (engaged && !(r[0x5e] & 1) && rec == 0) {             // the right mouse is the one hid lists
            std::memcpy(r + 0x50, &mi.cfg[0], 4);
            if (!g_was_engaged) {
                ++g_forced_on;
                // the game's pointer starts where the Windows pointer was (1920 x 1080 UI units; wider than 16:9 the UI is the centred
                // 16:9 part of the picture, ObeliskChamber_wide.cpp)
                float u = 0, v = 0;
                if (hid::jc2_mouse_host_pointer(&u, &v)) {
                    const WideInfo wi = wide_info();
                    const float k = wi.active && wi.k > 0 ? wi.k : 1.0f;
                    const int32_t x = static_cast<int32_t>(std::clamp(((u - 0.5f) * k + 0.5f) * 1920.0f, 0.0f, 1920.0f));
                    const int32_t y = static_cast<int32_t>(std::clamp(v * 1080.0f, 0.0f, 1080.0f));
                    std::memcpy(reinterpret_cast<void*>(mgr + 0xdc), &x, 4);
                    std::memcpy(reinterpret_cast<void*>(mgr + 0xe0), &y, 4);
                    if (tt::enabled()) NXLOG_I(kTag, "game pointer placed at the Windows pointer: %.3f,%.3f on the picture -> %d,%d", u, v, x, y);
                }
            }
        }
        if (lifted_now && !engaged && (r[0x5e] & 1)) {
            r[0x5e] = 0;
            const float zero = 0;
            std::memcpy(r + 0x4c, &zero, 4);
            std::memcpy(r + 0x50, &zero, 4);
            ++g_forced_off;
        }
    }
    g_was_engaged = engaged;
    // where the game's pointer is, for the Windows pointer when the mouse is freed (the inverse of the mapping above)
    if (mi.mode) {
        const WideInfo wi = wide_info();
        const float k = wi.active && wi.k > 0 ? wi.k : 1.0f;
        hid::jc2_mouse_set_return_point((static_cast<float>(mi.cursor_x) / 1920.0f - 0.5f) / k + 0.5f, static_cast<float>(mi.cursor_y) / 1080.0f);
    }
}

// Once per frame (GamePlay::frame): the handover, then the frames in mouse mode and each change in the log, so test runs show who has control.
void mouse_frame() {
    const uint64_t mgr = mouse_mgr();
    MouseInfo mi = mouse_info();
    if (!mi.valid) return;
    if (g_switch_now) {
        handover(mgr, mi);
        mi = mouse_info();
    }
    if (!g_cfg_logged) {
        g_cfg_logged = true;
        NXLOG_I(kTag, "game mouse controls: enabled %d, allowed %d; on after %.2f frames on the surface (or a move whose square is over %.0f), "
                "off %.2f frames after it is lifted (six-axis: %.2f, after %.2f frames); switch at once: %d", mi.enabled ? 1 : 0, mi.allowed ? 1 : 0, static_cast<double>(mi.cfg[0]),
                static_cast<double>(mi.cfg[1]), static_cast<double>(mi.cfg[4]), static_cast<double>(mi.cfg[2]), static_cast<double>(mi.cfg[3]), g_switch_now ? 1 : 0);
    }
    ++g_frames;
    if (mi.mode) ++g_frames_mouse;
    if (mi.mode != g_was) {
        (mi.mode ? g_entries : g_exits)++;
        const hid::Jc2Stats js = hid::jc2_mouse_stats();
        NXLOG_I(kTag, "game mouse controls %s (entries %llu, exits %llu; Joy-Con 2 mouse engaged %llu, lifted %llu; switched at once: on %llu, "
                "off %llu; game pointer %d,%d)", mi.mode ? "ON" : "off", static_cast<unsigned long long>(g_entries),
                static_cast<unsigned long long>(g_exits), static_cast<unsigned long long>(js.engages), static_cast<unsigned long long>(js.lifts),
                static_cast<unsigned long long>(g_forced_on), static_cast<unsigned long long>(g_forced_off), mi.cursor_x, mi.cursor_y);
        g_was = mi.mode;
    }
    if (tt::enabled() && mi.mode && (mi.dx || mi.dy)) {
        static uint32_t n = 0;
        // the mouse system (0x22983a8): +0x208.. the movement after the calibration angle, the situation on top of its stack (+0xa0 deque)
        uint64_t sys = 0, blocks = 0, start = 0, count = 0, blk = 0;
        float mv[4] = {};
        int32_t situation = -1;
        if (rd(g_base + 0x22983a8, &sys) && sys && mem::is_mapped(sys + 0x200, 0x18)) {
            std::memcpy(mv, reinterpret_cast<const void*>(sys + 0x208), sizeof mv);
            if (rd(sys + 0xa0, &blocks) && rd(sys + 0xb8, &start) && rd(sys + 0xc0, &count) && count) {
                const uint64_t i = start + count - 1;
                if (rd(blocks + (i >> 10) * 8, &blk)) rd(blk + (i & 0x3ff) * 4, &situation);
            }
        }
        if (n++ < 400 && (n < 40 || n % 10 == 0))
            NXLOG_I(kTag, "game mouse movement %d,%d pointer %d,%d; processed %.1f %.1f %.1f %.1f situation %d", mi.dx, mi.dy, mi.cursor_x,
                    mi.cursor_y, static_cast<double>(mv[0]), static_cast<double>(mv[1]), static_cast<double>(mv[2]), static_cast<double>(mv[3]),
                    situation);
    }
}

}  // namespace nxrt::games::ObeliskChamber
