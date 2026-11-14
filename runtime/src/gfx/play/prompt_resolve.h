// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts (S17): which icon of the prompt pack stands in for each of the game's own button icons. Pure functions, no
// game memory and no graphics; prompts.cpp does the texture side.
//
// A game draws its prompts ("PRESS (A) TO START", the footer of the menus, tutorial texts) with small textures, one per Switch button glyph
// (Icon below). Which textures those are, and how to find them in the game's files, is the game's (PromptGame, nxrt/game.h Info::prompts).
// Metroid Prime Remastered: 19 small BC7 textures TXTR_ButtonA ... TXTR_DPadIdle and the two stick icons in GuiSysMP1.pak and
// PreloadFrontEndMPT.pak (games/mpr/mpr_prompts.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "play_types.h"

namespace nxrt::play {

// The physical button an icon shows. None: the icon is not a button of the Switch pad (the stick icons).
enum class Btn : uint8_t { A, B, X, Y, L, R, ZL, ZR, Plus, Minus, DPad, None, kCount };

// The game's icon textures. The "Pressed" ones are the gold highlighted twins the game draws while the button is held.
enum class Icon : uint8_t {
    A, APressed, B, BPressed, X, Y, L, R, RPressed, ZL, ZLPressed, ZR, ZRPressed, Plus, PlusPressed, Minus, DPad, LStick, RStick,
    kCount
};
constexpr int kIconCount = static_cast<int>(Icon::kCount);

// ---- the game's side -----------------------------------------------------------------------------------------------------------------
struct PromptIcon {
    Btn btn;
    bool pressed;
    const char* asset;          // the texture's name in the game's files (logs): "TXTR_ButtonA"
    const char* uuid;           // its asset id there, canonical lower-case text
    const char* pak;            // the archive under the RomFS folder: "Preload/MPR1/GuiSysMP1.pak"
    const char* front_uuid;     // a second copy of the icon (Metroid: the front end's own, in front_pak), or nullptr
};
struct PromptGame {
    const PromptIcon* icons = nullptr;   // kIconCount entries in Icon order
    const char* front_pak = "";          // the archive of the second copies
    uint32_t icon_size = 0;              // width = height of the icon textures in pixels (single level)
    bool upside_down = false;            // the game stores its UI textures bottom row first (the pack's upright icon goes in with its rows reversed)
    // Reads every icon texture (both copies) from the game files under `romfs` and calls found() with each one's tiled bytes, as the game uploads
    // them. Returns how many were found.
    int (*scan)(const std::string& romfs, void (*found)(void* user, Icon icon, const uint8_t* tiled, size_t size), void* user) = nullptr;
};
const PromptGame* prompt_game();        // the registered game's, or nullptr (then the game's own icons stay)

Btn icon_button(Icon i);                // Btn::None without a game
bool icon_pressed(Icon i);
const char* icon_asset_name(Icon i);    // "" without a game
const char* icon_asset_uuid(Icon i);
const char* icon_pak_file(Icon i);
const char* icon_front_uuid(Icon i);    // nullptr when the icon has no second copy

struct ResolveIn {
    PromptStyle style = PromptStyle::Switch;   // never Auto: the caller resolves Auto first
    bool label_abxy = true;                    // [pad] label_abxy: Xbox A is Switch A (true) or by position (false)
    Ctx ctx = Ctx::Menu;                       // where the player is
    const Settings* settings = nullptr;        // bindings (keyboard style) and the pad remap (pad styles); null: the built-in mapping
};

// The name of the prompt pack entry that replaces the game's icon: "xbox/a", "ps/cross", "kbm/space", with "_pressed" appended for a
// pressed icon. Pad styles show the physical button that presses the Switch button, following settings->pad.remap. Empty: keep the
// game's own icon (Switch style, a keyboard style button no input is bound to, or a pad style where nothing presses the button or the
// pad source is a stick click or the d-pad).
// The stick icons (LStick, RStick) are the map's controls: keyboard style on the map shows the mouse buttons that drive it (left button
// rotates, middle button moves; kbm/mouse1 and kbm/mouse2). Every other style and context keeps the game's own stick icon.
std::string resolve_icon(const ResolveIn& in, Icon icon);

// Keyboard and mouse: the input that presses this Switch button in the given context, following the bindings of the settings.
// Looks at actions whose [switch] output is exactly that button (confirm and back before the raw btn_ actions), in the bindings that
// apply in ctx (a binding of the context beats the `all` layer for the same input), then in the other contexts. False: nothing is bound.
bool input_for_button(const Settings& s, Ctx ctx, Btn b, InputId* out);

// Pack entry name of an input: "kbm/w", "kbm/left ctrl", "kbm/mouse1", "kbm/wheelup".
std::string kbm_pack_name(const InputId& id);

// Pad styles (Xbox, PlayStation): the pack entry of the physical button that presses Switch button b ("ps/cross", "xbox/lb"), following the
// pad remap and the label question, as resolve_icon does for an icon. "" when nothing presses it or the source has no icon (a stick click).
// For games that draw their own prompt glyphs instead of PromptGame's textures (Fortune's Weave: font glyphs, games/ObeliskChamber/ObeliskChamber_prompts.cpp).
std::string pad_button_name(PromptStyle style, const PadSettings& pad, bool label_abxy, Btn b);

// Which style "auto" shows: keyboard and mouse once the last input came from them, else the kind of the pad used last.
// Pad kinds are input::PadKind values: 0 other, 1 Xbox, 2 PlayStation, 3 Switch. connected_pad_kind is -1 without a pad. Before any
// input: the first pad if one is connected, else the keyboard.
enum class LastDevice : uint8_t { None, KeyboardMouse, Pad };
PromptStyle auto_style(LastDevice last, int last_pad_kind, int connected_pad_kind);

}  // namespace nxrt::play
