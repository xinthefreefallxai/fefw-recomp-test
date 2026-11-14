// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the whole game module as one nxrt::game::Info (fefw_game.h). Play-mode hooks come here as they are found.
#include "fefw_game.h"

#include <string>
#include <vector>

#include "play_display.h"
#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::fefw {

namespace {

// ---- factory keyboard bindings (play_types.h settings file; README "Keyboard and controller", runtime/docs/CONFIG.md) ----------------------
// The game reads only the Switch pad, so every key presses a Switch button or pushes a stick (a key pushes it all the way). The game module
// has no menu or play detection, so play mode's context stays `menu` (play.cpp: no ProcessInput hook) and every binding is in
// [bindings.all], which works in menus and in the plaza alike. Clear of the runtime's own keys: F1 (this menu), F2 (frame rate counter),
// Alt+Enter (full screen). The first key of a button is the one the keyboard prompts show (prompt_resolve.cpp input_for_button).
struct FactoryKey {
    const char* key;      // SDL scancode name
    play::Act act;
    const char* action;   // the settings-file name of `act` (the generic one: fefw names no slots)
};
constexpr FactoryKey kFactoryKeys[] = {
    {"W", play::Act::MoveForward, "move_forward"},
    {"S", play::Act::MoveBack, "move_back"},
    {"A", play::Act::MoveLeft, "move_left"},
    {"D", play::Act::MoveRight, "move_right"},
    {"Up", play::Act::LookUp, "look_up"},
    {"Down", play::Act::LookDown, "look_down"},
    {"Left", play::Act::LookLeft, "look_left"},
    {"Right", play::Act::LookRight, "look_right"},
    {"I", play::Act::LookUp, "look_up"},
    {"K", play::Act::LookDown, "look_down"},
    {"J", play::Act::LookLeft, "look_left"},
    {"L", play::Act::LookRight, "look_right"},
    {"Return", play::Act::BtnA, "btn_a"},
    {"Space", play::Act::BtnA, "btn_a"},
    {"Keypad Enter", play::Act::BtnA, "btn_a"},
    {"Escape", play::Act::BtnB, "btn_b"},
    {"Backspace", play::Act::BtnB, "btn_b"},
    {"R", play::Act::BtnX, "btn_x"},
    {"F", play::Act::BtnY, "btn_y"},
    {"Q", play::Act::BtnL, "btn_l"},
    {"E", play::Act::BtnR, "btn_r"},
    {"Z", play::Act::BtnZL, "btn_zl"},
    {"C", play::Act::BtnZR, "btn_zr"},
    {"Tab", play::Act::BtnPlus, "btn_plus"},
    {"M", play::Act::BtnMinus, "btn_minus"},
    {"1", play::Act::DpadUp, "dpad_up"},
    {"2", play::Act::DpadDown, "dpad_down"},
    {"3", play::Act::DpadLeft, "dpad_left"},
    {"4", play::Act::DpadRight, "dpad_right"},
    {"Keypad 8", play::Act::DpadUp, "dpad_up"},
    {"Keypad 2", play::Act::DpadDown, "dpad_down"},
    {"Keypad 4", play::Act::DpadLeft, "dpad_left"},
    {"Keypad 6", play::Act::DpadRight, "dpad_right"},
    {"V", play::Act::BtnLStick, "btn_lstick"},
    {"B", play::Act::BtnRStick, "btn_rstick"},
};

// Revision 1 (0.1.5): the keyboard bindings above. Before it fefw had none, and the files the game wrote have empty [bindings.*] sections at
// defaults_rev 0, so each of them is a revision-1 addition: an older controls.ini gets every key it has not bound itself (in [bindings.all]),
// keys a player bound stay theirs (play_config.cpp apply_default_additions).
constexpr int kDefaultsRev = 1;

const play::GameDefaults& game_defaults() {
    static const play::GameDefaults d = [] {
        static std::string text;
        static std::vector<play::GameDefaultAddition> adds;
        text = "[meta]\ndefaults_rev = " + std::to_string(kDefaultsRev) + "\n\n[bindings.all]\n";
        for (const FactoryKey& k : kFactoryKeys) {
            text += std::string(k.key) + " = " + k.action + "\n";
            adds.push_back({kDefaultsRev, play::Ctx::All, k.key, k.act});
        }
        play::GameDefaults g;
        g.settings_text = text.c_str();
        g.defaults_rev = kDefaultsRev;
        g.additions = adds.data();
        g.addition_count = static_cast<unsigned>(adds.size());
        return g;
    }();
    return d;
}

// Frame update: the game's present wrapper (main 0x5d170: nvnWindowSetPresentInterval when the interval changed, nvnQueuePresentTexture),
// once per presented frame on the main thread, after the frame's input was read: it pumps play mode (window events, the gamepad into the
// npad state the next frame samples). Pacing stays in the present (nvn_present.cpp, the game's own rate): [latency] pace defaults to
// present for a game that sets present_interval_hz (extras/lowlat.cpp), so this hook never runs the limiter.
constexpr uint64_t kPresentWrapper = 0x5d170;

// play mode's install / remove / frame: the game hooks (fefw_play.cpp) and the button glyphs (fefw_prompts.cpp)
void install_all(uint64_t base, play::GameHost* host, const play::Settings& s) {
    play_install(base, host, s);
    mouse_install(base);
    sysmenu_install(base);
    optmenu_install(base);
    saveslot_install(base);
    modmenu_install(base);
    prompts_install(host);
}
void remove_all() {
    prompts_remove();
    play_remove();
}
void frame_all(bool, double, play::GameFrame*) {
    prompts_frame();
    mouse_frame();
    sysmenu_frame();
    optmenu_frame();
    saveslot_frame();
    modmenu_frame();
}

// The picture's shape for play mode's pointer geometry (window <-> picture, the Joy-Con 2 mouse's pointer hand-over): the output when it
// is wider than 16:9 (fefw_wide.cpp), else 16:9 (narrower outputs are fitted to 16:9, fefw_res.cpp).
play::DisplayLive display_live() {
    play::DisplayLive d;
    const WideInfo wi = wide_info();
    d.render_w = wi.active && wi.out_w > 0 ? wi.out_w : 1920;
    d.render_h = wi.active && wi.out_h > 0 ? wi.out_h : 1080;
    return d;
}

const play::GamePlay& game_play() {
    static const play::GamePlay t = [] {
        play::GamePlay p;
        p.frame_update = kPresentWrapper;
        p.install = &install_all;
        p.remove = &remove_all;
        p.frame = &frame_all;
        p.settings_menu = &settings_menu;
        p.display_live = &display_live;   // fefw_menu.cpp: the F1 menu in place of the base settings page
        return p;
    }();
    return t;
}

}  // namespace

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &game_play();
        g.play_defaults = &game_defaults();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::fefw

void nxrt_register_game_fefw() { nxrt::game::set_current(&nxrt::games::fefw::full_info()); }
