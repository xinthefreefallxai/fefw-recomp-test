// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (S8): the data model shared by the settings file, the binding engine and the settings page.
//
// Vocabulary:
//   input    a physical thing on the keyboard or mouse: a key (SDL scancode), a mouse button or a wheel notch.
//   action   what the player wants the game to do (Metroid Prime Remastered: "fire", "jump", "beam_2"). The game only knows the Switch pad,
//            so every action is tied to a Switch output (buttons and stick directions) by the [switch] table of the settings file.
//   context  where the player is: in play, in the alternate body (the morph ball), on the map, in a menu, inspecting (the scan visor).
//            `all` is a layer under every context.
// The action and context slots are generic; the registered game names them and gives their factory outputs and the launcher's labels
// (GameVocabulary below; Metroid Prime Remastered: games/mpr/mpr_vocabulary.cpp). Without a game the slots keep generic names.
//   trigger  how an input turns into an action: press (while held), tap (a short pulse after a quick release), hold (after a delay,
//            while held), toggle (each press flips it), doubletap (a second quick press, while held).
// A binding says "in this context, this input drives this action with this trigger".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/extras.h"
#include "play_fastboot.h"
#include "play_tweaks.h"

namespace nxrt::play {

// ---- contexts ----------------------------------------------------------------------------------------------------------------
// Play: the game in play. Alt: in play in the alternate body (a second control scheme). Inspect: in play with an inspect mode on.
enum class Ctx : uint8_t { All = 0, Play, Alt, Map, Menu, Inspect, kCount };
constexpr int kCtxCount = static_cast<int>(Ctx::kCount);
const char* ctx_name(Ctx c);                           // the game's names (mpr: "all" "gameplay" "morph" "map" "menu" "scan"); generic: "all" "play" "alt" "map" "menu" "inspect"
bool parse_ctx(const std::string& s, Ctx* out);        // case-insensitive
inline bool ctx_in_play(Ctx c) { return c == Ctx::Play || c == Ctx::Alt || c == Ctx::Inspect; }

// ---- triggers ----------------------------------------------------------------------------------------------------------------
enum class Trig : uint8_t { Press = 0, Tap, Hold, Toggle, DoubleTap };
const char* trig_name(Trig t);                         // "press" "tap" "hold" "toggle" "doubletap"
bool parse_trig(const std::string& s, Trig* out);      // case-insensitive; also accepts "double_tap"

// ---- actions -----------------------------------------------------------------------------------------------------------------
enum class Act : uint8_t {
    None = 0,
    // left stick
    MoveForward, MoveBack, MoveLeft, MoveRight,
    // right stick (keyboard look; the mouse does not use these)
    LookUp, LookDown, LookLeft, LookRight,
    // game actions (mpr: fire, lock on, missile, jump, morph ball, free look, map, pause, confirm, back)
    Primary, Target, Secondary, Jump, AltForm, LookHold, Map, Pause, Confirm, Back,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    SetA1, SetA2, SetA3, SetA4,            // select one of four in the cycled set (mpr: the beams power, wave, ice, plasma)
    SetB1, SetB2, SetB3, SetB4,            // select one of four in a second set (mpr: the visors combat, scan, thermal, x-ray)
    SetANext, SetAPrev,                    // cycle set A (the mouse wheel)
    // raw Switch buttons, for anything the table above does not cover
    BtnA, BtnB, BtnX, BtnY, BtnL, BtnR, BtnZL, BtnZR, BtnPlus, BtnMinus, BtnLStick, BtnRStick,
    // a game-side switch with no Switch output (pkp: the Joy-Con 2 mouse mode, toggled by each activation)
    MouseMode,
    kCount
};
constexpr int kActCount = static_cast<int>(Act::kCount);

struct ActInfo {
    Act act;
    const char* name;           // the name used in the settings file: "move_forward" "btn_zl"; the game's for its slots (mpr: "fire" "beam_1")
    const char* default_out;    // the default [switch] entry in output syntax (see parse_output): "ZR", "X+UP", "LS_UP" ... "" = none
};
const ActInfo& act_info(Act a);                         // the registered game's row (GameVocabulary::acts), else the generic one
bool parse_act(const std::string& name, Act* out);     // case-insensitive, '-' and '_' equal

// ---- Switch outputs ----------------------------------------------------------------------------------------------------------
// What an action does on the Switch pad: buttons pressed together plus stick deflection (each axis -1..1, summed over all active
// actions and clamped). SetANext / SetAPrev have no fixed output: the engine turns them into one of SetA1..4 (see engine).
struct Output {
    uint64_t buttons = 0;       // nn::hid::NpadButton bits (nxrt/input_source.h: btn(kBtnA) ...)
    float lx = 0, ly = 0;       // left stick, y up = +
    float rx = 0, ry = 0;       // right stick, y up = +
    bool empty() const { return buttons == 0 && lx == 0 && ly == 0 && rx == 0 && ry == 0; }
    bool operator==(const Output& o) const { return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry; }
};
// Syntax: terms joined by '+', case-insensitive: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT (d-pad) LSTICK RSTICK (clicks)
// and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT. Example "X+UP" = X and d-pad up together.
// "" or "none" parses to the empty output. Unknown terms: returns false and *err names the term.
bool parse_output(const std::string& text, Output* out, std::string* err = nullptr);
std::string output_text(const Output& o);              // canonical text, parse_output(output_text(o)) == o

// ---- inputs ------------------------------------------------------------------------------------------------------------------
enum class InKind : uint8_t { None = 0, Key, Mouse, Wheel };
struct InputId {
    InKind kind = InKind::None;
    int32_t code = 0;           // Key: SDL_Scancode. Mouse: 1 left, 2 middle, 3 right, 4 x1, 5 x2 (the numbering of input.h). Wheel: +1 up, -1 down.
    bool operator==(const InputId& o) const { return kind == o.kind && code == o.code; }
    bool operator<(const InputId& o) const { return kind != o.kind ? kind < o.kind : code < o.code; }
};
// Names: SDL's scancode names for keys ("W", "Space", "Left Ctrl", "Tab", "1", "Keypad 5", "F"), "Mouse1".."Mouse5", "WheelUp", "WheelDown".
std::string input_name(const InputId& id);
bool parse_input(const std::string& name, InputId* out);   // case-insensitive; false for an unknown name

struct Binding {
    InputId in;
    Ctx ctx = Ctx::Play;    // All = every context that has no binding of its own for the same input
    Act act = Act::None;
    Trig trig = Trig::Press;
    int ms = 0;                 // Tap/Hold/DoubleTap window in ms; 0 = the default of [timing]
};

// ---- settings ----------------------------------------------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
// Source engine yaw scale: one mouse count at sensitivity 1.0 turns 0.022 degrees. The settings file stores radians per count; the
// settings page shows (and edits) the Source-scale value, source = rad_per_count / kSourceRadPerCount.
constexpr double kSourceRadPerCount = 0.022 * kPi / 180.0;
inline double source_sens(double rad_per_count) { return rad_per_count / kSourceRadPerCount; }
inline double rad_per_count(double source) { return source * kSourceRadPerCount; }

struct MouseSettings {
    double yaw_rad = 0.000767945;   // radians per count, horizontal
    double pitch_rad = 0.000767945;   // radians per count, vertical (separate)
    double alt_yaw_rad = 0.000767945;     // the alternate body's camera (mpr: the morph ball camera): its own sensitivity, radians per count
    double alt_pitch_rad = 0.000767945;
    bool alt_camera = true;                        // the mouse and the right stick turn the alternate body's camera (mpr: the morph ball camera)
    bool invert_y = false;                         // false: mouse up looks up
    bool invert_x = false;
    bool menu_cursor = true;                       // in menus the mouse is a pointer: cursor, hover, click, right click, wheel
    bool wheel_cycles = true;                       // the wheel cycles set A (mpr: the beams; the SetANext / SetAPrev bindings stay either way)
    // The Joy-Con 2 mouse (hid jc2_mouse.h, [input] jc2_mouse): what the PC mouse sends the game as a Joy-Con 2 (R) mouse.
    double jc2_speed = 1.0;                         // pointer units per mouse count (the game's own mouse settings scale it further)
    double jc2_wake = 8.0;                          // mouse counts within 0.25 s that switch the game to its mouse controls
    bool jc2_capture = true;                        // capture the mouse while the game is in its mouse controls (raw motion, no OS cursor)
};
// ---- pad button remapping ------------------------------------------------------------------------------------------------------------
// The physical buttons of an SDL gamepad, named by position (the face buttons by where they sit, so the names do not depend on the printed
// letters). Each one presses a Switch button; `default` (kPadDefault) keeps the built-in mapping of pad_map.h.
enum class PadSrc : uint8_t {
    South = 0, East, West, North,                         // face buttons: bottom, right, left, top
    LeftShoulder, RightShoulder, LeftTrigger, RightTrigger,
    Start, Back, LeftStick, RightStick,                   // the two sticks are clicks
    DpadUp, DpadDown, DpadLeft, DpadRight,
    kCount
};
constexpr int kPadSrcCount = static_cast<int>(PadSrc::kCount);
const char* pad_src_name(PadSrc s);                      // the file's word: "south" "east" "west" "north" "left_shoulder" ... "dpad_right"
bool parse_pad_src(const std::string& name, PadSrc* out);   // case-insensitive, '-' and ' ' equal '_'
constexpr uint64_t kPadDefault = ~0ull;                  // PadSettings::remap value: the built-in mapping

struct PadSettings {
    uint64_t remap[kPadSrcCount] = {kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault,
                                    kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault};
    bool label_abxy = true;        // true: buttons by the label printed on them (Xbox A = Switch A); false: by position (bottom = Switch B)
    double stick_deadzone = 0.10;  // left stick, radial, 0..0.5, applied before the game's own dead zone
    double right_deadzone = 0.10;  // right stick (looking), same rule
    double stick_max = 1.0;        // left stick, 0.5..1: the fraction of the travel that counts as full deflection (1 = off). A pad whose stick tops out below 1 reaches full speed with this lowered
    bool invert_y = false;         // right stick up looks down (in play only: menus and the map keep the stick as it is). Stacks with the game's own option.
    double look_sens = 1.0;        // 0.25..3: the right stick's deflection is multiplied by this in play (below 1 the turn speed tops out lower, above 1 it tops out sooner)
    double trigger_threshold = 0.35;   // analog trigger travel (0..1) that counts as ZL / ZR pressed
    bool gyro = true;              // pass the gyro to the six-axis sensor
    double gyro_scale = 1.0;
    bool rumble = true;
    double rumble_scale = 1.0;
    // How the game's HD Rumble value reaches the pad (haptic.h): 0 auto (DualSense haptics on a wire, HD Rumble packets for a
    // Switch Pro Controller, the standard rumble for everything else), 1 off, 2 the standard rumble only, 3 DualSense haptics.
    // haptic_strength is 0..100 percent, or -1 to keep using rumble_scale above.
    int haptic = 0;
    int haptic_strength = -1;
    double alt_orbit = 2.0;       // rad/s at full right-stick deflection, 0 = off (0..6). The alternate body's camera only (mpr: morph ball).
};
struct TimingSettings {
    int tap_ms = 200;              // a press shorter than this is a tap
    int hold_ms = 250;             // a hold starts after this
    int double_ms = 300;           // the second press of a double tap must come within this of the first release
    int pulse_ms = 80;             // how long a tap / set A or B select (mpr: beam, visor) / wheel notch stays pressed on the Switch side
};
struct DisplaySettings {
    std::string window_mode = "windowed";   // "windowed" "borderless" "exclusive"
    bool vsync = false;            // false: no forced vsync (immediate / mailbox, VRR friendly) with the frame cap below
    // [display] fps_limit (older name fps_cap): 0 = unlimited (with vsync on: mailbox presents), -1 = auto (the display's refresh rate),
    // else the cap; with vsync on, a cap only applies when it is below the refresh rate
    int fps_cap = 60;
    int width = 1280, height = 720;    // windowed client size, remembered
    bool keep_aspect = true;       // letterbox instead of stretching
    // ---- S10 display options (play_display.h). The first four only apply at the next start: the game builds its render targets once.
    // Internal resolution in percent (25..400), 0 = auto; ignored with a fixed size. What 100 means is the game's: the window's client size
    // where the game module resizes the targets itself (auto = 100), the console's own size where the NVN back end scales the game's
    // targets (Pokopia: 100 = 1920x1080, auto = the window height / 1080, at least 1).
    int render_scale = 100;
    int render_width = 0, render_height = 0;   // fixed internal resolution (both > 0 and both within 320..16384), 0 = follow the window
    double aspect = 0;             // 0 = the ratio of the internal resolution; otherwise the forced ratio of the 3D view and the HUD (1.0 .. 5.0)
    // live:
    double fov_offset = 0;         // degrees added to the vertical field of view of every game camera (-30 .. 40), 0 = the game's own
    bool dynamic_resolution = false;   // the game's own dynamic resolution (it lowers the render size when the GPU is late)
    int anisotropy = 0;            // 0 = what the game's samplers ask for; 2, 4, 8 or 16 = at least that on every filtering sampler
    double lod_bias = 0;           // texture LOD bias added to every sampler (-2.0 .. 2.0; negative is sharper)
    bool bloom = true;
    bool chromatic_aberration = true;
    // Games with quality hooks (play_game.h kDisplayOptQuality, live): the view distance multiplies the camera's far clip (1.0 .. 4.0, 1 = the
    // console's), the preset names the set of values last picked on the F1 page (switch2, high, ultra; custom after a single change).
    std::string quality_preset = "switch2";
    double view_distance = 1.0;
    // ---- S38 GPU paths, at the next start; [nvn] pool_mirror and pass_barriers in mpr.ini or the environment win over these.
    bool fast_gpu_memory = true;   // device-local copies of the pools draws read ([nvn] pool_mirror = on); costs up to 2 GiB of video memory
    bool fewer_gpu_waits = true;   // no full barrier after passes that only wrote their attachments ([nvn] pass_barriers = tracked)
    bool shader_prewarm = true;    // build the pipelines recorded by earlier runs ahead of the draws ([nvn] prewarm); at the next start, [nvn] prewarm in the ini wins
    std::string astc_quality = "high";   // ASTC texture quality on this GPU: fast, high or uncompressed (RGBA8, lots of video memory); becomes [gfx] astc_quality at the next start unless the ini or the environment set it
    // The display the window lives on, 1-based in the system's order; 0 = automatic (wherever the window opens). Applied live (the window moves).
    int monitor = 0;
};

// ---- audio -------------------------------------------------------------------------------------------------------------------------
struct AudioSettings {
    double volume = 1.0;           // [audio] volume, master gain 0..1, applied live; [audio] volume in the ini or the environment wins at the start
    bool mute_unfocused = false;   // [audio] mute_unfocused: silent while the window has no focus (live)
    std::string device = "default";   // [audio] device: "default" or part of an output device's name; used at the next start ([audio] device in the ini wins)
    int channels = 2;              // [audio] channels: 2 (stereo) or 6 (5.1 surround) for the game's audio renderer; used at the next start ([audio] channels in the ini wins)
};

// ---- button prompts ----------------------------------------------------------------------------------------------------------------
// Which button icons the game's prompts show: the game's own Switch icons, or those of another pad or the keyboard. Auto follows the
// last input device used.
enum class PromptStyle : uint8_t { Switch = 0, Xbox, PlayStation, Keyboard, Auto };
constexpr int kPromptStyleCount = 5;
const char* prompt_style_name(PromptStyle s);          // "switch" "xbox" "playstation" "keyboard" "auto"
bool parse_prompt_style(const std::string& s, PromptStyle* out);   // case-insensitive; also accepts ps, ps4, ps5, dualsense, kbm, keyboard_mouse, mouse
struct PromptSettings {
    PromptStyle style = PromptStyle::Auto;     // follows the last used device: keyboard icons until a pad is used (or when none is connected)
};

// ---- frame rate counter ------------------------------------------------------------------------------------------------------------
// The small corner overlay with the frame rate and the frame time. Off by default; a hotkey (and the F1 page and the launcher) switches it.
enum class Corner : uint8_t { TopLeft = 0, TopRight, BottomLeft, BottomRight };
const char* corner_name(Corner c);                       // "top_left" "top_right" "bottom_left" "bottom_right"
bool parse_corner(const std::string& s, Corner* out);    // case-insensitive, '-' and ' ' equal '_'
constexpr int32_t kScancodeF2 = 59;                      // SDL_SCANCODE_F2
struct FpsSettings {
    bool show = false;                                   // [fps] show
    InputId key = {InKind::Key, kScancodeF2};            // [fps] key: the hotkey that flips `show` (a keyboard key; never reaches the game)
    Corner corner = Corner::TopLeft;                     // [fps] corner
    int detail = 1;                                      // [fps] detail: 0 frame rate only, 1 frame rate and frame time, 2 also the 1% low and the worst frame
    bool in_screenshots = false;                         // [fps] in_screenshots: the counter is drawn into screenshots too (test tooling asks for this)
};

// ---- lock-on and camera feel (S32) --------------------------------------------------------------------------------------------
// [aim] lock_mode: classic is the game's own lock-on (the view turns to the target); free starts the same way (the game centres the target)
// and then lets the mouse and the right stick move the view, as long as the target stays on the screen (free_lock_margin).
// [camera] gun_drives_view: 1 is the game's own behaviour (the gun's idle drift, walking bob and landing dip move the view); 0 keeps the view still.
enum class LockMode : uint8_t { Classic = 0, Free };
const char* lock_mode_name(LockMode m);                       // "classic" "free"
bool parse_lock_mode(const std::string& s, LockMode* out);    // case-insensitive
struct AimSettings {
    LockMode lock_mode = LockMode::Classic;
    double free_lock_margin = 0.15;   // [aim] free_lock_margin (0..0.4): with the free lock the view may move, but the locked target stays inside the screen with this
                                      // fraction of the screen size kept free along every edge (0 = right at the edge, 0.15 = the middle 70 percent)
};
struct CameraSettings {
    bool gun_drives_view = true;   // [camera] gun_drives_view: the arm cannon is fixed to the view, so its idle drift, walking bob and landing dip move the view
};

struct Settings {
    MouseSettings mouse;
    AimSettings aim;
    CameraSettings camera;
    TweakSettings tweaks;          // [tweaks] (play_tweaks.h): optional looks, off by default
    PadSettings pad;
    TimingSettings timing;
    DisplaySettings display;
    AudioSettings audio;
    extras::Settings extras;       // the [hdr] [upscale] [latency] [picture] sections (nxrt/extras.h): HDR output, upscaling, low latency
    PromptSettings prompts;
    FpsSettings fps;
    FastbootSettings fastboot;     // [tweaks] skip_intro skip_boot_logos fast_menus menu_speed (play_fastboot.h, S46)
    Output out[kActCount];         // the [switch] table; defaulted from ActInfo::default_out
    std::vector<Binding> bindings;
    // [meta] defaults_rev: the factory-defaults revision the file was written with (0 when the file has no [meta]). When a file with
    // bindings sections is read at an older revision, the newer factory bindings are added to it (play_config.cpp defaults_rev(): the game's GameDefaults::rev and additions).
    int defaults_rev = 0;
    // Transient, never written to the file: set by load_settings when it upgraded an older file, so the caller can keep a .bak and save.
    bool migrated = false;
    std::string migration_added;   // transient: the bindings the upgrade added, for the log line
};

// The factory settings: the table above and the default bindings (default_settings_text(), which is the same text the settings file would
// contain). The text, its revision and the bindings later revisions add are the registered game's (nxrt/game.h Info::play_defaults;
// Metroid Prime Remastered: games/mpr/mpr_defaults.cpp); without one the text is empty and the revision 0.
Settings default_settings();
const char* default_settings_text();

// A factory binding that a later revision of the defaults added: a controls.ini that has [bindings.*] sections and a defaults_rev below
// `rev` gets it (only where the input is still unbound in that context, so the user's own choice wins).
struct GameDefaultAddition {
    int rev;
    Ctx ctx;
    const char* input;
    Act act;
};
// A factory setting that a later revision changed: a controls.ini below `rev` that still has the old factory value `from` takes `to` once
// (a value the player picked themselves stays).
struct GameDefaultChange {
    int rev;
    const char* section;   // "aim"
    const char* key;       // "lock_mode"
    const char* from;
    const char* to;
};
struct GameVocabulary;
struct GameDefaults {
    const char* settings_text = "";                  // the factory settings file
    int defaults_rev = 0;                            // the [meta] defaults_rev this build writes; bump it together with a new addition
    const GameDefaultAddition* additions = nullptr;
    unsigned addition_count = 0;
    const GameVocabulary* vocabulary = nullptr;      // the names of the action and context slots (below)
    const struct GameDefaultChange* changes = nullptr;
    unsigned change_count = 0;
};
const GameDefaults* game_defaults();                 // the registered game's, or nullptr

// The game's words for the generic slots (Info::play_defaults->vocabulary). Every part is optional: a null table keeps the generic names.
struct GameActLabel {
    Act act;
    const char* label;          // the launcher's row label (UTF-8): "Fire", "Power beam"
    const char* group;          // the launcher's group (UTF-8): "Combat", "Beams"
};
struct GameVocabulary {
    const ActInfo* acts = nullptr;                   // kActCount rows in enum order: the file names and factory outputs of every action
    const char* const* ctx_names = nullptr;          // kCtxCount names in enum order
    const GameActLabel* labels = nullptr;            // the launcher's Keyboard rows, in display order
    unsigned label_count = 0;
    // settings-file keys and launcher labels of the alternate-body and cycle settings (nullptr: the generic key)
    const char* key_alt_yaw = nullptr;               // [mouse] (mpr: "morph_yaw"; generic "alt_yaw")
    const char* key_alt_pitch = nullptr;             // [mouse] (mpr: "morph_pitch"; generic "alt_pitch")
    const char* key_wheel_cycles = nullptr;          // [mouse] (mpr: "wheel_beams"; generic "wheel_cycles")
    const char* key_alt_orbit = nullptr;             // [pad]   (mpr: "ball_orbit"; generic "alt_orbit")
    const char* key_alt_camera = nullptr;            // [mouse] on/off (mpr: "ball_camera"; generic "alt_camera")
    const char* key_alt_sens = nullptr;              // [mouse] one sensitivity for both directions, Source scale (mpr: "ball_sensitivity"; generic "alt_sensitivity")
    const char* label_alt_camera = nullptr;          // launcher slider (mpr: "Morph ball camera")
    const char* label_wheel_cycles = nullptr;        // launcher check box (mpr: "Mouse wheel changes the beam")
    const char* label_alt_camera_on = nullptr;       // launcher check box (mpr: "Mouse and right stick turn the morph ball camera")
};
const GameVocabulary* game_vocabulary();             // the registered game's, or nullptr
// The settings-file keys of the alternate-body and cycle settings: the game's, else the generic ones.
const char* key_alt_yaw();
const char* key_alt_pitch();
const char* key_wheel_cycles();
const char* key_alt_orbit();
const char* key_alt_camera();
const char* key_alt_sens();

// ---- the settings file ---------------------------------------------------------------------------------------------------------
// Plain ini, '#' or ';' starts a comment. Sections and keys are case-insensitive, whitespace around names and values is ignored.
//   [meta]     defaults_rev = <n>   (written by the game; the factory-defaults revision of the file, a file without it counts as 0)
//   [mouse]    yaw = 0.000768   pitch = 0.000768   invert_y = 0   invert_x = 0   menu_cursor = 1   wheel_beams = 1     (radians per count)
//              yaw_source / pitch_source = 2.0 are accepted as well (Source scale); when both forms appear the last one wins
//              alt_camera = 1 (the alternate body's camera follows the mouse and the right stick), alt_sensitivity = 2.0 (its sensitivity, Source
//              scale, both directions; alt_yaw / alt_pitch in radians per count set one direction each)
//   [pad]      label_abxy deadzone stick_max trigger_threshold gyro gyro_scale rumble rumble_scale alt_orbit
//   [timing]   tap_ms hold_ms double_ms pulse_ms
//   [display]  window_mode vsync fps_limit (number, 0 = unlimited, auto = refresh rate; fps_cap is read too) width height keep_aspect render_scale render_width render_height aspect fov_offset
//              dynamic_resolution anisotropy lod_bias bloom chromatic_aberration fast_gpu_memory fewer_gpu_waits shader_prewarm astc_quality (fast high uncompressed)
//              monitor (auto or the display's number, 1 = the first)
//   [hdr]      mode (off auto on) format (auto hdr10 scrgb) peak_nits (0 = auto, else 200..10000) paper_white_nits (0 = auto, else
//              80..500) highlight_boost (0..2)
//   [upscale]  mode (off fsr1 nis, and fsr3 dlss xess kept but not used; spatial = fsr1) sharpness (0..1) fsr_dll dlss_dll xess_dll (paths, kept verbatim)
//              frame_generation (1/0)
//   [latency]  mode (off auto on boost) log_timings (1/0)
//   [picture]  contrast (0.5..1.5) saturation (0..2) black (-0.5..0.25) gamma (0.5..2), neutral 1 1 0 1; preset (neutral vivid) is read, not written
//   [audio]    volume (0..1) mute_unfocused (1/0) device (default or part of an output device's name)
//   [prompts]  style = switch|xbox|playstation|keyboard|auto   (which button icons the game's prompts show; auto, the default, follows the last used input device)
//   [tweaks]   skip_intro skip_boot_logos fast_menus (1/0) menu_speed (1..4)   (play_fastboot.h)
//              glow_color (off | R,G,B) suit_glow ball_glow gun_glow (auto | off | R,G,B)   (play_tweaks.h)
//   [fps]      show (1/0) key (a keyboard key, F2) corner (top_left top_right bottom_left bottom_right) detail (0..2) in_screenshots (1/0)
//   [padmap]   <south east west north left_shoulder right_shoulder left_trigger right_trigger start back left_stick right_stick dpad_up dpad_down
//              dpad_left dpad_right> = default | none | <Switch buttons joined by '+', e.g. A or ZL+L>   (the pad button presses those Switch buttons)
//   [switch]   <action> = <output>      e.g.  fire = ZR    beam_1 = X+UP    (missing actions keep the default)
//   [bindings.<ctx>]   <input> = <action> [trigger [ms]]     e.g.   Mouse1 = fire    Left Ctrl = morph hold 250    Space = jump toggle
//                      ctx is a context name (mpr: all gameplay morph map menu scan). The same input may appear several times in a section.
//   The action, context and [mouse]/[pad] alternate-body key names in this description are Metroid Prime Remastered's (GameVocabulary).
// Unknown sections/keys are ignored with a warning in *warnings (not an error); a malformed value of a known key is an error.
// parse_settings starts from default_settings() and applies the text on top. If the text has any [bindings.*] section, the default
// bindings are dropped first (the file is then the whole truth); a file without bindings sections keeps the default bindings.
bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings = nullptr);
std::string format_settings(const Settings& s);        // canonical text, parse_settings(format_settings(s)) reproduces s
bool load_settings(const std::string& path, Settings* out, std::string* err);   // false + defaults in *out when the file is missing (err empty) or invalid (err set)
bool save_settings(const std::string& path, const Settings& s, std::string* err);   // writes atomically (temp file + rename)

}  // namespace nxrt::play
