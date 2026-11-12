// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the "Controls" window of the F1 overlay. The contract is play_ui.h. The page edits a working copy of the
// settings: Apply and save hands it to the host, Revert and Factory defaults replace the copy, and nothing else reaches the game.
#include "play_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::play {

namespace {

namespace hid = nxrt::hid;

const ImVec4 kRed(1.0f, 0.35f, 0.35f, 1.0f);
const ImVec4 kYellow(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kOrange(1.0f, 0.65f, 0.25f, 1.0f);
const ImVec4 kPressed(0.20f, 0.60f, 0.30f, 1.0f);
const ImVec4 kReleased(0.25f, 0.25f, 0.28f, 1.0f);

constexpr int kWindowModeCount = 3;
constexpr const char* kWindowModeNames[kWindowModeCount] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
constexpr const char* kWindowModeKeys[kWindowModeCount] = {"windowed", "borderless", "exclusive"};
constexpr int kSizePresets[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr int kSizePresetCount = static_cast<int>(sizeof kSizePresets / sizeof kSizePresets[0]);
constexpr int kFpsChoices[] = {-1, 0, 30, 60, 72, 90, 120, 144, 165, 240, 360};   // -1 = the refresh rate
// The aspect ratio combo: Auto stores 0, the presets store their ratio, Custom shows a slider (index kAspectCustom, no value of its own).
constexpr const char* kAspectNames[] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 1.6, 21.0 / 9.0, 32.0 / 9.0};
constexpr int kAspectCustom = 5;
constexpr int kAspectNameCount = static_cast<int>(sizeof kAspectNames / sizeof kAspectNames[0]);
constexpr const char* kAnisoNames[] = {"Game default", "2x", "4x", "8x", "16x"};
constexpr int kAnisoValues[] = {0, 2, 4, 8, 16};
constexpr int kAnisoCount = static_cast<int>(sizeof kAnisoValues / sizeof kAnisoValues[0]);
constexpr const char* kAstcValues[] = {"fast", "high", "uncompressed"};
constexpr const char* kAstcNames[] = {"Fast", "High (sharper, built in the background)", "Uncompressed (lots of video memory)"};
constexpr int kAstcCount = static_cast<int>(sizeof kAstcValues / sizeof kAstcValues[0]);
constexpr Trig kTrigChoices[] = {Trig::Press, Trig::Tap, Trig::Hold, Trig::Toggle, Trig::DoubleTap};
// The Button prompts combo on the Controller tab, in PromptStyle order.
constexpr const char* kPromptLabels[kPromptStyleCount] = {"Switch (the game's own)", "Xbox", "PlayStation", "Keyboard and mouse",
                                                          "Auto (follows the last used device)"};

struct SwitchButton {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchButton kSwitchButtons[] = {
    {"A", hid::kBtnA},         {"B", hid::kBtnB},         {"X", hid::kBtnX},         {"Y", hid::kBtnY},
    {"L", hid::kBtnL},         {"R", hid::kBtnR},         {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},
    {"PLUS", hid::kBtnPlus},   {"MINUS", hid::kBtnMinus}, {"UP", hid::kBtnUp},       {"DOWN", hid::kBtnDown},
    {"LEFT", hid::kBtnLeft},   {"RIGHT", hid::kBtnRight}, {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR},
};
constexpr int kSwitchButtonCount = static_cast<int>(sizeof kSwitchButtons / sizeof kSwitchButtons[0]);
static_assert(kSwitchButtonCount == 16, "the Switch pad has 16 buttons in the status view");

// The working copy and the editing state of the page. One page per process.
struct Page {
    bool loaded = false;
    bool dirty = false;                    // the working copy differs from the settings in force (set by every change)
    Settings edit;
    Ctx ctx = Ctx::Play;               // the context shown in the Bindings tab
    int rebind = -1;                       // index into edit.bindings of the row waiting for an input; -1 = none
    bool link_pitch = true;                // Mouse tab: moving the horizontal sensitivity moves the vertical one too (local, not saved)
    bool out_tab_open = false;             // the Switch layout tab was open last frame; its fields are refilled when it opens
    char out_text[kActCount][128] = {};    // the text of each output field
    std::string out_err[kActCount];        // parse error of each field; empty when its text is valid
    bool aspect_custom = false;            // Display tab: the Custom aspect entry is picked (local, not saved)
    bool fps_capture = false;              // Display tab: the frame rate counter's hotkey waits for a key (local, not saved)
    std::string notice;                    // a line under the buttons: a refused or conflicting binding (cleared by the next rebind)
    bool notice_warn = false;              // the notice is a warning (drawn in orange), not information
    std::vector<std::string> monitors;     // Display page: "1: name (w x h @ hz)", refreshed every couple of seconds while the page is open
    double monitors_at = -1e9;
    std::vector<std::string> audio_devices;   // Audio page: output device names, read when the page opens and by its Refresh button
    bool audio_loaded = false;
};

void mark(Page& p) { p.dirty = true; }

const char* yes_no(bool b) { return b ? "yes" : "no"; }
const char* cstr(const char* s) { return s ? s : ""; }
const char* act_label(Act a) { return a == Act::None ? "none (input off)" : act_info(a).name; }

// Stops our rebind. The host's capture is cancelled only when it is ours.
void stop_rebind(UiHost& host, Page& p) {
    if (p.rebind >= 0 && host.capturing()) host.cancel_capture();
    p.rebind = -1;
}

// Ends the frame rate counter's hotkey capture. The host's capture is cancelled only when it is ours.
void stop_fps_capture(UiHost& host, Page& p) {
    if (p.fps_capture && host.capturing()) host.cancel_capture();
    p.fps_capture = false;
}

// The bindings of the working copy that share row `row`'s input in the same context and trigger: pressing that input would do both.
std::vector<int> conflicts_of(const Page& p, int row) {
    std::vector<int> out;
    const Binding& a = p.edit.bindings[static_cast<std::size_t>(row)];
    if (a.in.kind == InKind::None) return out;
    for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
        const Binding& b = p.edit.bindings[i];
        if (static_cast<int>(i) != row && b.ctx == a.ctx && b.in == a.in && b.trig == a.trig) out.push_back(static_cast<int>(i));
    }
    return out;
}

void set_notice(Page& p, const std::string& text, bool warn) {
    p.notice = text;
    p.notice_warn = warn;
}

// Takes the captured input, if the host has one. Results are taken every frame, so a stale one never reaches a later rebind.
// Only one capture runs at a time (the binding row or the frame rate hotkey), so a result goes to whichever one is waiting.
// The key that opens the menu cannot be bound (and the frame rate hotkey cannot be bound to an action): the page says so and keeps waiting.
void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        const std::string name = input_name(id);
        const bool reserved = host.reserved_key(id);
        if (p.fps_capture) {
            // The frame rate hotkey takes keyboard keys only; a mouse button or a wheel notch is dropped and the capture ends.
            if (reserved) {
                set_notice(p, name + " opens this menu and cannot be the frame rate hotkey. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.fps_capture = false;
            if (id.kind == InKind::Key) {
                Settings cur = host.settings();
                cur.fps.key = id;
                host.apply(cur);   // live: not part of the working copy
            }
        } else if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && id == cur.fps.key)) {
                set_notice(p, name + (reserved ? " opens this menu" : " is the frame rate counter hotkey") + " and cannot be bound. Press another input, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
            const std::vector<int> other = conflicts_of(p, p.rebind);
            if (other.empty()) {
                set_notice(p, name + " bound.", false);
            } else {
                const Binding& o = p.edit.bindings[static_cast<std::size_t>(other[0])];
                set_notice(p, name + " is also bound to " + act_label(o.act) + " (" + ctx_name(o.ctx) + "): both happen when it is pressed.", true);
            }
        }
        p.rebind = -1;
    } else if ((p.rebind >= 0 || p.fps_capture) && !host.capturing()) {
        p.rebind = -1;   // Escape, or the host ended the capture without a result
        p.fps_capture = false;
        set_notice(p, "Rebinding cancelled.", false);
    }
}

// Fills the Switch layout fields from the working copy. Also clears their errors.
void refresh_out_text(Page& p) {
    for (int a = 1; a < kActCount; ++a) {   // Act::None has no row
        const std::string text = output_text(p.edit.out[a]);
        std::snprintf(p.out_text[a], sizeof p.out_text[a], "%s", text.c_str());
        p.out_err[a].clear();
    }
}

// Changes made by the page that must hold before the copy is handed to the host: no bindings without an input, values in range.
void sanitize(Settings& s) {
    std::vector<Binding>& v = s.bindings;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& b) { return b.in.kind == InKind::None; }), v.end());
    for (Binding& b : v) b.ms = std::clamp(b.ms, 0, 5000);
    s.display.width = std::clamp(s.display.width, 320, 16384);
    s.display.height = std::clamp(s.display.height, 320, 16384);
    // A fixed render size is 0 (follow the window) or 320..16384 per side; the settings file rejects anything else.
    s.display.render_width = s.display.render_width <= 0 ? 0 : std::clamp(s.display.render_width, 320, 16384);
    s.display.render_height = s.display.render_height <= 0 ? 0 : std::clamp(s.display.render_height, 320, 16384);
    // Extras: peak and paper white are 0 (auto) or in range; the upscale mode is kept as it is, even for the modes not yet offered.
    extras::HdrSettings& h = s.extras.hdr;
    h.peak_nits = h.peak_nits <= 0 ? 0 : std::clamp(h.peak_nits, 200.0, 10000.0);
    h.paper_white_nits = h.paper_white_nits <= 0 ? 0 : std::clamp(h.paper_white_nits, 80.0, 500.0);
    h.highlight_boost = std::clamp(h.highlight_boost, 0.0, 2.0);
    s.extras.upscale.sharpness = std::clamp(s.extras.upscale.sharpness, 0.0, 1.0);
    s.extras.picture = extras::picture_clamped(s.extras.picture);
    s.audio.volume = std::clamp(s.audio.volume, 0.0, 1.0);
    s.display.monitor = std::clamp(s.display.monitor, 0, 32);
    if (s.audio.device.empty()) s.audio.device = "default";
}

// The frame rate counter is live (the hotkey changes it in the game), so the working copy always takes it from the host, never from a
// stale copy: Apply and save, Revert and Factory defaults leave the counter as it is.
void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit.fps = host.settings().fps;
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);   // the colour sliders preview live: back to the saved values
    refresh_out_text(p);
    p.dirty = false;
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);
    refresh_out_text(p);
    mark(p);
}

// A slider on a double; true when the user changed it.
bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags = 0) {
    float f = static_cast<float>(*v);
    if (!ImGui::SliderFloat(label, &f, static_cast<float>(lo), static_cast<float>(hi), format, flags)) return false;
    *v = f;
    return true;
}

// An int field. The value is clamped when the field is left, not while typing (typing "1280" must not turn into "320" on the way).
bool int_field(const char* label, int* v, int lo, int hi, int step, int step_fast) {
    bool changed = ImGui::InputInt(label, v, step, step_fast);
    if (ImGui::IsItemDeactivated()) {
        const int c = std::clamp(*v, lo, hi);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// One side of the fixed render size: 0 (follow the window) or 320..16384. A value in between is clamped to 320 when the field is left.
bool render_size_field(const char* label, int* v) {
    bool changed = ImGui::InputInt(label, v, 0, 0);
    if (ImGui::IsItemDeactivated()) {
        const int c = *v <= 0 ? 0 : std::clamp(*v, 320, 16384);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// Sizes are in units of the 16 px reference font, so the page follows the overlay's UI scale (overlay.cpp).
float px(float v) { return v * ImGui::GetFontSize() / 16.0f; }

// A hover tooltip for the item drawn last.
void tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", text);
}

// Marks the item drawn last as one that only takes effect at the next start.
void restart_badge() {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "restart");
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies on restart: the game reads this once, when it starts.");
}

// ---- Mouse --------------------------------------------------------------------------------------------------------------------------

void sensitivity_help(double rad) {
    if (rad <= 0) {
        ImGui::TextDisabled("= %.6f rad per count", rad);
        return;
    }
    ImGui::TextDisabled("= %.6f rad per count, %.3f degrees per count, 360 degrees in %.0f counts", rad, rad * 180.0 / kPi, 2.0 * kPi / rad);
}

// The game's own entries of the Mouse tab (play_game.h GamePlay::mouse_tab): where = 0 after the sensitivity, 1 after the invert boxes, 2 at the end.
void game_mouse_section(int where, Page& p) {
    const GamePlay* gp = game_play();
    if (gp && gp->mouse_tab && gp->mouse_tab(where, p.edit)) mark(p);
}

void mouse_tab(Page& p) {
    MouseSettings& m = p.edit.mouse;
    double src = source_sens(m.yaw_rad);
    if (slider("Horizontal sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.yaw_rad = rad_per_count(src);
        if (p.link_pitch) m.pitch_rad = m.yaw_rad;
        mark(p);
    }
    sensitivity_help(m.yaw_rad);

    src = source_sens(m.pitch_rad);
    if (slider("Vertical sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.pitch_rad = rad_per_count(src);
        mark(p);
    }
    sensitivity_help(m.pitch_rad);
    ImGui::Checkbox("Link vertical to horizontal", &p.link_pitch);
    game_mouse_section(0, p);

    ImGui::Spacing();
    if (ImGui::Checkbox("Invert vertical", &m.invert_y)) mark(p);
    if (ImGui::Checkbox("Invert horizontal", &m.invert_x)) mark(p);
    game_mouse_section(1, p);
    if (ImGui::Checkbox("Mouse pointer in menus (cursor, hover, click)", &m.menu_cursor)) mark(p);
    game_mouse_section(2, p);
}

// ---- Controller ---------------------------------------------------------------------------------------------------------------------

// The 16 controller buttons, in PadSrc order.
constexpr const char* kPadSrcLabels[kPadSrcCount] = {
    "A / Cross (bottom)", "B / Circle (right)", "X / Square (left)", "Y / Triangle (top)",
    "LB / L1",            "RB / R1",            "LT / L2",            "RT / R2",
    "Menu / Options",     "View / Create",      "Left stick click",   "Right stick click",
    "D-pad up",           "D-pad down",         "D-pad left",         "D-pad right",
};
static_assert(sizeof kPadSrcLabels / sizeof kPadSrcLabels[0] == 16, "one label per controller button");

// The choices of a controller button: Default, Nothing, then the single Switch buttons in kSwitchButtons order.
constexpr const char* kPadChoiceNames[] = {
    "Default", "Nothing",
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick click", "Right stick click",
};
constexpr int kPadChoiceCount = static_cast<int>(sizeof kPadChoiceNames / sizeof kPadChoiceNames[0]);
static_assert(kPadChoiceCount == 2 + kSwitchButtonCount, "one choice per Switch button, plus Default and Nothing");
constexpr const char* kPadCustomLabel = "Custom (edit in controls.ini)";

// The value a choice stores.
uint64_t pad_choice_value(int c) {
    if (c == 0) return kPadDefault;
    if (c == 1) return 0;
    return static_cast<uint64_t>(hid::btn(kSwitchButtons[c - 2].bit));
}

// The choice a stored value shows as, or -1 when no choice holds it (a mask such as two buttons, set in the file).
int pad_choice_of(uint64_t v) {
    for (int c = 0; c < kPadChoiceCount; ++c)
        if (pad_choice_value(c) == v) return c;
    return -1;
}

void pad_buttons_group(Page& p) {
    ImGui::SeparatorText("Pad buttons");
    ImGui::TextWrapped("What each controller button presses on the Switch pad. Default keeps the built-in layout.");
    if (ImGui::BeginTable("pad_buttons", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Controller button");
        ImGui::TableSetupColumn("Presses on the Switch pad");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kPadSrcCount; ++i) {
            uint64_t& v = p.edit.pad.remap[i];
            const int sel = pad_choice_of(v);
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPadSrcLabels[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##remap", sel >= 0 ? kPadChoiceNames[sel] : kPadCustomLabel)) {
                if (sel < 0) {   // a mask no choice holds: shown as the current entry, not offered
                    ImGui::BeginDisabled();
                    ImGui::Selectable(kPadCustomLabel, true);
                    ImGui::EndDisabled();
                }
                for (int c = 0; c < kPadChoiceCount; ++c) {
                    if (ImGui::Selectable(kPadChoiceNames[c], c == sel) && c != sel) {
                        v = pad_choice_value(c);
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset pad buttons")) {
        bool changed = false;
        for (uint64_t& v : p.edit.pad.remap) {
            if (v != kPadDefault) {
                v = kPadDefault;
                changed = true;
            }
        }
        if (changed) mark(p);
    }
}

// The button prompt style: which icons the game shows (Auto follows the last device used). Shared by the keyboard and the pad.
void prompts_row(const LiveStatus& live, Page& p) {
    PromptSettings& prompts = p.edit.prompts;
    const int prompt_sel = static_cast<int>(prompts.style);
    if (ImGui::BeginCombo("Button prompts", kPromptLabels[prompt_sel])) {
        for (int i = 0; i < kPromptStyleCount; ++i) {
            if (ImGui::Selectable(kPromptLabels[i], i == prompt_sel) && i != prompt_sel) {
                prompts.style = static_cast<PromptStyle>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("The button icons in the game's prompts: the game's own Switch icons, Xbox, PlayStation, keyboard and mouse, or Auto to follow the device you used last.");
    if (prompts.style == PromptStyle::Auto) {
        ImGui::SameLine();
        ImGui::TextDisabled("showing %s", cstr(live.prompt_now));
    }
}

void pad_tab(const LiveStatus& live, Page& p) {   // the Gamepad tab of the Controls page
    PadSettings& pad = p.edit.pad;
    const bool by_label = pad.label_abxy;
    if (ImGui::RadioButton("Buttons by label (Xbox A = Switch A)", by_label) && !by_label) {
        pad.label_abxy = true;
        mark(p);
    }
    if (ImGui::RadioButton("Buttons by position (bottom button = Switch B)", !by_label) && by_label) {
        pad.label_abxy = false;
        mark(p);
    }

    ImGui::SeparatorText("Sticks and triggers");
    if (slider("Left stick dead zone", &pad.stick_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The stick must move this far from the centre before it counts (a fraction of its travel). Applies before the game's own dead zone.");
    if (slider("Right stick dead zone", &pad.right_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The same for the right stick (looking).");
    if (slider("Left stick max", &pad.stick_max, 0.5, 1.0, "%.2f")) mark(p);
    tip("How far the left stick must be pushed to count as fully pushed (a fraction of its travel). Lower it if your stick never reaches full speed (for example it tops out around 0.95).");
    if (ImGui::Checkbox("Invert look (right stick up looks down)", &pad.invert_y)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies in play only; adds to the game's own invert option.");
    if (slider("Look sensitivity", &pad.look_sens, 0.25, 3.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Multiplies the right stick while playing. Below 1 the top turning speed is lower; above 1 you reach it with less stick.");
    if (slider("Trigger threshold", &pad.trigger_threshold, 0.05, 1.0, "%.2f")) mark(p);
    if (ImGui::Checkbox("Gyro (six-axis sensor)", &pad.gyro)) mark(p);
    if (slider("Gyro scale", &pad.gyro_scale, 0.1, 4.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::Checkbox("Rumble", &pad.rumble)) mark(p);
    if (slider("Rumble scale", &pad.rumble_scale, 0.0, 2.0, "%.2f")) mark(p);

    pad_buttons_group(p);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro available: %s    Rumble available: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Display ------------------------------------------------------------------------------------------------------------------------

int window_mode_index(const std::string& mode) {
    for (int i = 0; i < kWindowModeCount; ++i)
        if (mode == kWindowModeKeys[i]) return i;
    return 0;
}

// The combo entry for an aspect value. Auto wins for 0; Custom is shown when picked or when the value is no preset.
int aspect_index(double aspect, bool custom) {
    if (aspect <= 0.0) return 0;
    if (custom) return kAspectCustom;
    for (int i = 1; i < kAspectCustom; ++i)
        if (std::fabs(aspect - kAspectValues[i]) < 1e-6) return i;
    return kAspectCustom;
}

void fps_label(int fps, char* buf, std::size_t n) {
    if (fps < 0) std::snprintf(buf, n, "Refresh rate");
    else if (fps == 0) std::snprintf(buf, n, "Unlimited");
    else std::snprintf(buf, n, "%d fps", fps);
}

// The frame rate counter. Its controls act on the host at once, not on the working copy: the game's hotkey flips `show` too, so the
// page reads the host's value every frame and never writes a stale copy. These controls do not mark the page as changed.
constexpr const char* kCornerNames[] = {"Top left", "Top right", "Bottom left", "Bottom right"};   // in Corner order
constexpr int kCornerCount = static_cast<int>(sizeof kCornerNames / sizeof kCornerNames[0]);
constexpr const char* kFpsDetailNames[] = {"Frame rate", "Frame rate and frame time", "Plus 1% low and worst frame"};
constexpr int kFpsDetailCount = static_cast<int>(sizeof kFpsDetailNames / sizeof kFpsDetailNames[0]);

void set_fps_live(UiHost& host, const FpsSettings& f) {
    Settings cur = host.settings();
    cur.fps = f;
    host.apply(cur);
}

void fps_group(UiHost& host, Page& p) {
    const FpsSettings fps = host.settings().fps;
    ImGui::SeparatorText("Frame rate counter");

    bool show = fps.show;
    if (ImGui::Checkbox("Show frame rate counter", &show)) {
        FpsSettings f = fps;
        f.show = show;
        set_fps_live(host, f);
    }

    const int corner = static_cast<int>(fps.corner);
    if (ImGui::BeginCombo("Corner##fps_corner", kCornerNames[corner])) {
        for (int i = 0; i < kCornerCount; ++i) {
            if (ImGui::Selectable(kCornerNames[i], i == corner) && i != corner) {
                FpsSettings f = fps;
                f.corner = static_cast<Corner>(i);
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    const int detail = std::clamp(fps.detail, 0, kFpsDetailCount - 1);
    if (ImGui::BeginCombo("Detail##fps_detail", kFpsDetailNames[detail])) {
        for (int i = 0; i < kFpsDetailCount; ++i) {
            if (ImGui::Selectable(kFpsDetailNames[i], i == detail) && i != detail) {
                FpsSettings f = fps;
                f.detail = i;
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    bool shots = fps.in_screenshots;
    if (ImGui::Checkbox("Also draw it in screenshots", &shots)) {
        FpsSettings f = fps;
        f.in_screenshots = shots;
        set_fps_live(host, f);
    }

    const std::string key = input_name(fps.key);
    ImGui::Text("Hotkey: %s", key.empty() ? "?" : key.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Change##fps_key")) {
        stop_rebind(host, p);
        p.fps_capture = true;
        host.begin_capture();
    }
    if (p.fps_capture) {
        ImGui::SameLine();
        ImGui::TextDisabled("press a keyboard key...");
    }
}

// ---- Display page groups (HDR, upscaling, low latency) ----------------------------------------------------------------------------------

// HDR output, upscaling and low latency (nxrt/extras.h). Items that apply at the next start say so in their labels, as the Display tab does.
constexpr const char* kHdrModeNames[] = {"Off", "Auto", "On", "Native"};
constexpr const char* kHdrFormatNames[] = {"Auto", "HDR10", "scRGB"};
constexpr const char* kLatencyModeNames[] = {"Off", "Auto", "On", "Boost"};
constexpr const char* kUpscaleNames[] = {"Off", "FSR 1 (spatial)", "FSR 3", "DLSS", "XeSS", "NIS (spatial)", "DLAA"};   // indexed by extras::UpscaleMode
// The order the list shows them in: the spatial upscalers, then the temporal ones.
constexpr extras::UpscaleMode kUpscaleOrder[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nis,
                                                 extras::UpscaleMode::Fsr3, extras::UpscaleMode::Dlss, extras::UpscaleMode::Dlaa, extras::UpscaleMode::Xess};
constexpr const char* kUpscaleQualityNames[] = {"Native (AA only)", "Quality", "Balanced", "Performance", "Ultra performance", "Custom (scene resolution)"};

// Off and the spatial upscalers (FSR 1, NIS) always work. The temporal ones (src/gfx/extras/temporal.h) get depth, camera motion vectors made by
// the runtime and the game's projection jitter: FSR 3 when the FidelityFX DLLs are found, DLSS / DLAA when the Streamline DLLs and nvngx_dlss.dll are found (the GPU
// check runs when the renderer starts them). XeSS is not wired. A file that holds an unavailable one shows it as selected (the game runs as Off).
bool upscale_available(extras::UpscaleMode m) {
    switch (m) {
        case extras::UpscaleMode::Off:
        case extras::UpscaleMode::Fsr1:
        case extras::UpscaleMode::Nis: return true;
        case extras::UpscaleMode::Fsr3: return extras::temporal_ffx_available();
        case extras::UpscaleMode::Dlss:
        case extras::UpscaleMode::Dlaa: return extras::temporal_dlss_available();
        default: return false;
    }
}
const char* temporal_why(extras::UpscaleMode m) {
    if (m == extras::UpscaleMode::Fsr3)
        return "Needs amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (AMD FidelityFX SDK v2.4.0) in upscalers\\fsr3 next to the "
               "executable, or a path in [upscale] fsr_dll.";
    if (m == extras::UpscaleMode::Dlss || m == extras::UpscaleMode::Dlaa)
        return "Needs an NVIDIA RTX GPU and the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) in upscalers\\streamline "
               "next to the launcher, or their folder in [upscale] dlss_dll. They come from NVIDIA's Streamline SDK (bin/x64); this project does not ship them.";
    return "XeSS is not wired up yet.";
}

// A nits field where 0 means "auto" (shown as "auto"). Other values are clamped into lo..hi when the field is left, so the slider can be
// dragged through the gap below lo while it is held.
bool auto_nits_slider(const char* label, double* v, int lo, int hi) {
    int x = static_cast<int>(std::lround(*v));
    bool changed = ImGui::SliderInt(label, &x, 0, hi, x <= 0 ? "auto" : "%d nits");
    if (ImGui::IsItemDeactivated()) {
        const int c = x <= 0 ? 0 : std::clamp(x, lo, hi);
        changed = changed || c != x;
        x = c;
    }
    if (changed) *v = x;
    return changed;
}

void hdr_group(Page& p) {
    extras::HdrSettings& h = p.edit.extras.hdr;
    ImGui::SeparatorText("HDR output");

    const int mode = static_cast<int>(h.mode);
    const bool mode_open = ImGui::BeginCombo("Mode##hdr_mode", kHdrModeNames[mode]);
    tip("Off keeps the SDR picture, Auto uses HDR when the display and Windows offer it, On runs the HDR pipeline even without an HDR display. "
        "Native is On with the levels the game declares for an HDR TV on Switch 2.");
    if (mode_open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kHdrModeNames[i], i == mode) && i != mode) {
                h.mode = static_cast<extras::HdrMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    const int format = static_cast<int>(h.format);
    const bool format_open = ImGui::BeginCombo("Format##hdr_format", kHdrFormatNames[format]);
    tip("The signal sent to the display: HDR10, or scRGB; Auto picks HDR10 when the display offers it and scRGB otherwise.");
    if (format_open) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kHdrFormatNames[i], i == format) && i != format) {
                h.format = static_cast<extras::HdrFormat>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (auto_nits_slider("Peak brightness##hdr_peak", &h.peak_nits, 200, 10000)) mark(p);
    tip("How bright the brightest highlights get, in nits; auto reads the peak brightness the display reports.");
    if (auto_nits_slider("Paper white##hdr_paper", &h.paper_white_nits, 80, 500)) mark(p);
    tip("The brightness of ordinary white in the game, in nits; auto follows Windows' SDR content brightness when HDR is on there, else 203.");
    if (slider("Highlight boost", &h.highlight_boost, 0.0, 2.0, "%.2f")) mark(p);
    tip("0 keeps the game's SDR look up to its own white, 1 lets highlights reach the display's peak, 2 pushes them further.");

    const extras::HdrState hs = extras::hdr_state();
    ImGui::Text("HDR scene: %s, swapchain: %s, paper white %.0f nits, peak %.0f nits, peak ratio %.2f, display reports HDR: %s",
                hs.scene_hdr ? "on" : "off", extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits),
                static_cast<double>(hs.peak_nits), static_cast<double>(hs.peak_ratio), yes_no(hs.display_hdr));
}

void upscale_group(Page& p) {
    extras::UpscaleSettings& u = p.edit.extras.upscale;
    ImGui::SeparatorText("Upscaling");

    // Only the modes that work are offered. A file that holds another one shows it as the current entry, marked, until the player picks.
    const int mode = static_cast<int>(u.mode);
    std::string preview = kUpscaleNames[mode];
    if (!upscale_available(u.mode)) preview += " (not available)";
    const bool open = ImGui::BeginCombo("Upscaler##upscale_mode", preview.c_str());
    tip("FSR 1 (AMD) and NIS (NVIDIA Image Scaling) enlarge the game's picture from a lower render or scene resolution and sharpen it; both run on any GPU. "
        "NIS covers up to 2x per side, FSR 1 takes over beyond that. FSR 3 and DLSS / DLAA are temporal: they anti-alias the HDR scene using depth, "
        "camera motion and the game's jitter (switched on with them); DLSS and DLAA need an RTX GPU and apply from the next start.");
    if (open) {
        for (extras::UpscaleMode m : kUpscaleOrder) {
            const int i = static_cast<int>(m);
            const bool ok = upscale_available(m);
            if (!ok) ImGui::BeginDisabled();
            if (ImGui::Selectable(kUpscaleNames[i], i == mode) && i != mode && ok) {
                u.mode = m;
                mark(p);
            }
            if (!ok) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", temporal_why(m));
            }
        }
        ImGui::EndCombo();
    }
    if ((u.mode == extras::UpscaleMode::Dlss || u.mode == extras::UpscaleMode::Dlaa) && !extras::temporal_dlss_started()) restart_badge();

    if (u.mode == extras::UpscaleMode::Fsr3 || u.mode == extras::UpscaleMode::Dlss) {
        const int q = static_cast<int>(u.quality);
        if (ImGui::BeginCombo("Quality##upscale_quality", kUpscaleQualityNames[q])) {
            for (int i = 0; i < 6; ++i) {
                if (ImGui::Selectable(kUpscaleQualityNames[i], i == q) && i != q) {
                    u.quality = static_cast<extras::UpscaleQuality>(i);
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The scene's render size per side: Native 100%, Quality 67%, Balanced 59%, Performance 50%, Ultra performance 33%, Custom the scene resolution "
            "below. Takes effect where the game renders its scene below the output size; otherwise the upscaler runs as native anti-aliasing.");
    }

    if (u.mode == extras::UpscaleMode::Dlss || u.mode == extras::UpscaleMode::Dlaa || extras::temporal_dlss_started()) {
        static const int kPresets[] = {0, 11, 10, 12, 13, 5, 6};
        static const char* const kPresetNames[] = {"Default (the DLSS file decides)", "K (transformer)", "J (transformer)", "L (transformer, 2nd generation)",
                                                   "M (transformer, 2nd generation, faster)", "E (older model, lightest)", "F (older model)"};
        int cur = 0;
        for (int i = 0; i < 7; ++i)
            if (kPresets[i] == u.dlss_preset) cur = i;
        if (ImGui::BeginCombo("DLSS model##dlss_preset", kPresetNames[cur])) {
            for (int i = 0; i < 7; ++i) {
                if (ImGui::Selectable(kPresetNames[i], i == cur) && i != cur) {
                    u.dlss_preset = kPresets[i];
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The neural network DLSS and DLAA run. The transformer models (K, J, L, M) look best and cost the most GPU time, above all on RTX 20 and 30 cards; "
            "E and F are the older, lighter models. A DLSS file that no longer has the chosen model uses its default. Applies live.");
    }

    if (slider("Sharpness", &u.sharpness, 0.0, 1.0, "%.2f")) mark(p);
    tip("How much FSR 1, NIS or FSR 3 sharpens the upscaled picture; 1 is the sharpest (DLSS has no sharpening).");
    if (ImGui::SliderInt("Scene resolution##upscale_scene", &u.scene_scale, 25, 100, "%d%%")) mark(p);
    tip("Renders the 3D scene at this percent of the output size per side; the game scales it up and keeps the HUD and the post effects at full size. 100 turns it off. Applies live.");
    ImGui::TextDisabled("Or render everything below the window size with the render scale above; the upscaler then fills the rest.");
}

void latency_group(Page& p) {
    extras::LatencySettings& l = p.edit.extras.latency;
    ImGui::SeparatorText("Low latency");   // NVIDIA Reflex or AMD Anti-Lag 2 (nxrt/lowlat.h)

    const int mode = static_cast<int>(l.mode);
    const bool open = ImGui::BeginCombo("Mode##latency_mode", kLatencyModeNames[mode]);
    tip("Lowers the delay from input to picture with NVIDIA Reflex or AMD Anti-Lag 2, whichever the GPU has; Boost asks Reflex for its boost mode.");
    if (open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kLatencyModeNames[i], i == mode) && i != mode) {
                l.mode = static_cast<extras::LatencyMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (ImGui::SliderInt("Frames ahead##latency_ahead", &l.frames_ahead, -1, 3, l.frames_ahead < 0 ? "no limit" : "%d")) mark(p);
    tip("With the mode on, the game starts simulating a frame only when at most this many earlier frames are still on their way to the screen. 1 keeps the game's frame queue short; 0 is the shortest delay and costs frame rate.");
    restart_badge();
    if (ImGui::Checkbox("Log timings##latency_log", &l.log_timings)) mark(p);
    tip("Writes one line of latency timings to the log every second.");
    restart_badge();

    if (const extras::LowLatency* ll = extras::lowlat()) {
        const extras::LowLatency::Stats st = ll->stats();
        ImGui::Text("%s: enabled %s, input to present %.2f ms, driver input to GPU end %.2f ms, sleep %.2f ms", ll->backend_name(),
                    yes_no(st.enabled), st.input_to_present_ms, st.input_to_gpu_ms, st.sleep_ms_avg);
    } else {
        ImGui::TextDisabled("Not running: the renderer has not started its low-latency part yet, or the mode is off.");
    }
}

// The monitor list ("1: name (1920x1080 @ 144 Hz)"), read again every couple of seconds while the page is open.
void refresh_monitors(Page& p) {
    const double now = ImGui::GetTime();
    if (p.monitors_at >= 0.0 && now - p.monitors_at < 2.0) return;
    p.monitors_at = now;
    p.monitors.clear();
    const int n = platform::display_count();
    for (int i = 1; i <= n; ++i) {
        char buf[192];
        if (platform::display_name(i, buf, sizeof buf)) p.monitors.emplace_back(buf);
    }
}

void window_group(const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Window");
    const int mode = window_mode_index(d.window_mode);
    if (ImGui::BeginCombo("Window mode", kWindowModeNames[mode])) {
        for (int i = 0; i < kWindowModeCount; ++i) {
            if (ImGui::Selectable(kWindowModeNames[i], i == mode) && i != mode) {
                d.window_mode = kWindowModeKeys[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Borderless fullscreen keeps the desktop resolution and switches instantly. Exclusive fullscreen takes over the display. Alt+Enter flips between windowed and fullscreen in the game.");

    refresh_monitors(p);
    {
        char fallback[48];
        const char* preview = "Automatic (where the window is)";
        if (d.monitor > 0) {
            if (d.monitor <= static_cast<int>(p.monitors.size())) {
                preview = p.monitors[static_cast<std::size_t>(d.monitor - 1)].c_str();
            } else {
                std::snprintf(fallback, sizeof fallback, "%d: not connected", d.monitor);
                preview = fallback;
            }
        }
        if (ImGui::BeginCombo("Monitor", preview)) {
            if (ImGui::Selectable("Automatic (where the window is)", d.monitor <= 0) && d.monitor > 0) {
                d.monitor = 0;
                mark(p);
            }
            for (std::size_t i = 0; i < p.monitors.size(); ++i) {
                const int number = static_cast<int>(i) + 1;
                if (ImGui::Selectable(p.monitors[i].c_str(), d.monitor == number) && d.monitor != number) {
                    d.monitor = number;
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The display the window moves to. Fullscreen modes use this display.");
    }

    int preset = -1;
    for (int i = 0; i < kSizePresetCount; ++i)
        if (kSizePresets[i][0] == d.width && kSizePresets[i][1] == d.height) preset = i;
    char size_label[48];
    std::snprintf(size_label, sizeof size_label, "%d x %d%s", d.width, d.height, preset < 0 ? " (custom)" : "");
    if (ImGui::BeginCombo("Window size", size_label)) {
        for (int i = 0; i < kSizePresetCount; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "%d x %d", kSizePresets[i][0], kSizePresets[i][1]);
            if (ImGui::Selectable(name, i == preset) && i != preset) {
                d.width = kSizePresets[i][0];
                d.height = kSizePresets[i][1];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("The size of the game window in windowed mode. It changes at once when the window is windowed; in fullscreen it is the size the window returns to.");
    if (int_field("Width##win_w", &d.width, 320, 16384, 1, 100)) mark(p);
    if (int_field("Height##win_h", &d.height, 320, 16384, 1, 100)) mark(p);
    if (live.width > 0 && (live.width != d.width || live.height != d.height)) {
        if (ImGui::Button("Use the current window size")) {
            d.width = live.width;
            d.height = live.height;
            mark(p);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("now %d x %d", live.width, live.height);
    }
    if (ImGui::Checkbox("Keep the 16:9 picture, letterbox the rest", &d.keep_aspect)) mark(p);
}

void framerate_group(UiHost& host, const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Frame rate");
    if (ImGui::Checkbox("Vsync", &d.vsync)) mark(p);
    tip("Off: no forced vsync, lowest latency, works well with G-Sync / FreeSync together with a frame limit. On: the display paces the game.");

    char cap[32];
    fps_label(d.fps_cap, cap, sizeof cap);
    if (ImGui::BeginCombo("Frame limit", cap)) {
        for (int fps : kFpsChoices) {
            char name[32];
            fps_label(fps, name, sizeof name);
            if (ImGui::Selectable(name, fps == d.fps_cap) && fps != d.fps_cap) {
                d.fps_cap = fps;
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Refresh rate follows the display. With vsync on, a limit only counts when it is below the refresh rate.");
    ImGui::BeginDisabled(live.refresh_hz <= 0.0f);
    if (ImGui::Button("Just under the refresh rate")) {
        d.fps_cap = static_cast<int>(std::floor(nxrt::suggest_vrr_cap(live.refresh_hz)));
        mark(p);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (live.refresh_hz > 0.0f) ImGui::TextDisabled("display %.2f Hz", live.refresh_hz);
    else ImGui::TextDisabled("display refresh rate unknown");

    fps_group(host, p);
}

void resolution_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Resolution");
    bool autos = d.render_scale <= 0;
    if (ImGui::Checkbox("Automatic render scale", &autos)) {
        d.render_scale = autos ? 0 : 100;
        mark(p);
    }
    tip("Auto renders at the size of the window (never below the game's own 1080p). Turn it off to pick a percentage.");
    restart_badge();
    if (!autos) {
        if (ImGui::SliderInt("Render scale", &d.render_scale, 25, 400, "%d%%")) mark(p);
        tip("The game's internal resolution as a percentage of its own size (100% = 1920 x 1080). Above 100% renders more pixels than the console did.");
        restart_badge();
    }
}

// ---- Graphics -----------------------------------------------------------------------------------------------------------------------

// The game's quality preset (kDisplayOptQuality): picking one writes its view distance, texture LOD bias and anisotropy; changing one of
// those afterwards makes it Custom.
constexpr const char* kQualityIds[] = {"switch2", "high", "ultra", "custom"};
constexpr const char* kQualityNames[] = {"Switch 2 (console)", "High", "Ultra", "Custom"};
constexpr int kQualityCount = 4;

void quality_preset_combo(Page& p, const GamePlay* gp) {
    DisplaySettings& d = p.edit.display;
    int sel = kQualityCount - 1;
    for (int i = 0; i < kQualityCount; ++i)
        if (d.quality_preset == kQualityIds[i]) sel = i;
    if (ImGui::BeginCombo("Quality preset", kQualityNames[sel])) {
        for (int i = 0; i < kQualityCount - 1; ++i) {
            if (ImGui::Selectable(kQualityNames[i], i == sel) && i != sel && gp->quality_preset && gp->quality_preset(kQualityIds[i], d)) mark(p);
        }
        ImGui::EndCombo();
    }
    tip("Sets the view distance, texture LOD bias and anisotropy together. Switch 2 is the console's picture.");
}

void picture_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    const GamePlay* gp = game_play();
    const unsigned opts = gp ? gp->display_options : 0u;
    ImGui::SeparatorText("Picture");

    if (opts & kDisplayOptAspect) {
        const int aspect_sel = aspect_index(d.aspect, p.aspect_custom);
        if (ImGui::BeginCombo("Aspect ratio", kAspectNames[aspect_sel])) {
            for (int i = 0; i < kAspectNameCount; ++i) {
                if (ImGui::Selectable(kAspectNames[i], i == aspect_sel) && i != aspect_sel) {
                    if (i == kAspectCustom) {
                        p.aspect_custom = true;
                        if (d.aspect <= 0.0) d.aspect = kAspectValues[1];   // Custom starts at 16:9
                    } else {
                        p.aspect_custom = false;
                        d.aspect = kAspectValues[i];                       // Auto is index 0, value 0
                    }
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        restart_badge();
        if (aspect_sel == kAspectCustom && slider("Custom aspect ratio", &d.aspect, 1.0, 5.0, "%.2f")) mark(p);
    }

    ImGui::TextUnformatted("Fixed render size (0 = follow the window)");
    ImGui::SetNextItemWidth(px(120.0f));
    if (render_size_field("##render_w", &d.render_width)) mark(p);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(120.0f));
    if (render_size_field("##render_h", &d.render_height)) mark(p);
    restart_badge();
    tip("Renders at exactly this size instead of the window size times the render scale (Display page).");

    if (opts & kDisplayOptFov) {
        if (slider("Field of view offset (degrees)", &d.fov_offset, -30.0, 40.0, "%.1f")) mark(p);
        tip("Degrees added to the vertical field of view of every game camera. 0 is the game's own.");
    }
    if (ImGui::Checkbox("Dynamic resolution", &d.dynamic_resolution)) mark(p);
    tip("The game's own dynamic resolution: it lowers the render size when the GPU is late. Off keeps the full size.");

    const bool quality = (opts & kDisplayOptQuality) != 0;
    if (quality) quality_preset_combo(p, gp);
    auto custom = [&] {
        if (quality) d.quality_preset = "custom";
        mark(p);
    };
    if (quality) {
        if (slider("View distance", &d.view_distance, 1.0, 4.0, "x%.2f")) custom();
        tip("Multiplies the camera's far clip: terrain and large scenery reach farther. 1 is the console's.");
    }
    int aniso_sel = 0;
    for (int i = 0; i < kAnisoCount; ++i)
        if (kAnisoValues[i] == d.anisotropy) aniso_sel = i;
    if (ImGui::BeginCombo("Texture anisotropy", kAnisoNames[aniso_sel])) {
        for (int i = 0; i < kAnisoCount; ++i) {
            if (ImGui::Selectable(kAnisoNames[i], i == aniso_sel) && i != aniso_sel) {
                d.anisotropy = kAnisoValues[i];
                custom();
            }
        }
        ImGui::EndCombo();
    }
    tip("Sharper textures at a slant. Game default keeps what the game asks for; the other entries are a minimum on every filtering sampler.");
    if (slider("Texture LOD bias", &d.lod_bias, -2.0, 2.0, "%.2f")) custom();
    tip("Negative values pick sharper texture levels, positive values softer ones.");
    if (opts & kDisplayOptBloom) {
        if (ImGui::Checkbox("Bloom", &d.bloom)) mark(p);
    }
    if (opts & kDisplayOptChromatic) {
        if (ImGui::Checkbox("Chromatic aberration", &d.chromatic_aberration)) mark(p);
    }
}

void gpu_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("GPU and shaders");
    int astc_sel = 1;
    for (int i = 0; i < kAstcCount; ++i)
        if (d.astc_quality == kAstcValues[i]) astc_sel = i;
    if (ImGui::BeginCombo("Texture compression quality", kAstcNames[astc_sel])) {
        for (int i = 0; i < kAstcCount; ++i) {
            if (ImGui::Selectable(kAstcNames[i], i == astc_sel) && i != astc_sel) {
                d.astc_quality = kAstcValues[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("The game's textures are ASTC, which this GPU cannot sample, so they are converted. Fast: the quick BC7 conversion. High: the same at first, then a background thread "
        "re-encodes each texture slowly into the texture cache, so later runs are sharper (more disk space, no frame cost). Uncompressed: RGBA8, sharpest, 4 times the video memory "
        "of BC7. [gfx] astc_quality in the ini wins over this.");
    if (ImGui::Checkbox("Faster GPU memory (uses more video memory)", &d.fast_gpu_memory)) mark(p);
    tip("Keeps device-local copies of the memory the draws read: faster, costs up to 2 GiB of video memory. [nvn] pool_mirror in the ini wins over this.");
    restart_badge();
    if (ImGui::Checkbox("Fewer GPU pipeline waits", &d.fewer_gpu_waits)) mark(p);
    tip("No full barrier after render passes that only wrote their own targets. [nvn] pass_barriers in the ini wins over this.");
    restart_badge();
    if (ImGui::Checkbox("Prepare shaders ahead (pipeline prewarm)", &d.shader_prewarm)) mark(p);
    tip("Builds the pipelines that earlier runs recorded when the game loads a program, on background threads, so draws do not stall on a first compile. [nvn] prewarm in the ini wins over this.");
    restart_badge();
    if (const int left = gfx::shader_backlog(); left > 0) ImGui::TextDisabled("Preparing shaders: %d left", left);
    else ImGui::TextDisabled("No shaders waiting to be prepared");
}

// Colour controls of the present step (nxrt/extras.h PictureSettings). They show live while dragged; Apply and save keeps them.
void color_group(Page& p) {
    extras::PictureSettings& c = p.edit.extras.picture;
    ImGui::SeparatorText("Colour");
    bool changed = false;
    if (ImGui::Button("Neutral##pic_neutral")) changed = extras::picture_preset("neutral", &c);
    tip("The game's own picture, untouched.");
    ImGui::SameLine();
    if (ImGui::Button("Vivid##pic_vivid")) changed = extras::picture_preset("vivid", &c);
    tip("A modest boost: contrast 1.10, saturation 1.15, black level -0.03.");
    changed |= slider("Contrast##pic_contrast", &c.contrast, 0.5, 1.5, "%.2f");
    tip("Spreads the tones away from mid grey; 1 is the game's own.");
    changed |= slider("Saturation##pic_saturation", &c.saturation, 0.0, 2.0, "%.2f");
    tip("Colour intensity; 0 is grey, 1 is the game's own.");
    changed |= slider("Black level##pic_black", &c.black, -0.5, 0.25, "%.3f");
    tip("Below 0 pulls the darkest tones down to black (against a hazy, washed-out look); above 0 lifts them. White stays. 0 is the game's own.");
    changed |= slider("Gamma##pic_gamma", &c.gamma, 0.5, 2.0, "%.2f");
    tip("Mid-tone brightness: above 1 brightens, below 1 darkens. Black and white stay. 1 is the game's own.");
    if (changed) {
        extras::set_picture_live(c);
        mark(p);
    }
    ImGui::TextDisabled(c.neutral() ? "Neutral: the picture is presented as the game drew it." : "Applies live; Apply and save keeps it.");
}

void graphics_page(Page& p) {
    hdr_group(p);
    picture_group(p);
    color_group(p);
    gpu_group(p);
}

// ---- Audio --------------------------------------------------------------------------------------------------------------------------

void load_audio_devices(Page& p) {
    p.audio_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) p.audio_devices.push_back(d.name);
    p.audio_loaded = true;
}

void audio_page(Page& p) {
    AudioSettings& a = p.edit.audio;
    ImGui::SeparatorText("Volume");
    float pct = static_cast<float>(a.volume * 100.0);
    if (ImGui::SliderFloat("Master volume", &pct, 0.0f, 100.0f, "%.0f%%")) {
        a.volume = std::clamp(static_cast<double>(pct), 0.0, 100.0) / 100.0;
        mark(p);
    }
    tip("The volume of everything the game plays, applied as a gain in the audio output. [audio] volume in the ini or NXRT_AUDIO_VOLUME wins at the start.");
    if (ImGui::Checkbox("Mute when the window is not in focus", &a.mute_unfocused)) mark(p);

    ImGui::SeparatorText("Output");
    if (!p.audio_loaded) load_audio_devices(p);
    const bool is_default = a.device.empty() || a.device == "default";
    const char* preview = is_default ? "System default" : a.device.c_str();
    if (ImGui::BeginCombo("Output device", preview)) {
        if (ImGui::Selectable("System default", is_default) && !is_default) {
            a.device = "default";
            mark(p);
        }
        for (const std::string& name : p.audio_devices) {
            if (ImGui::Selectable(name.c_str(), !is_default && a.device == name) && a.device != name) {
                a.device = name;
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("Where the sound plays. The game opens its audio device once, so a new choice takes effect at the next start. [audio] device in the ini wins over this.");
    const bool surround = a.channels == 6;
    if (ImGui::BeginCombo("Speakers", surround ? "5.1 surround" : "Stereo")) {
        if (ImGui::Selectable("Stereo", !surround) && surround) {
            a.channels = 2;
            mark(p);
        }
        if (ImGui::Selectable("5.1 surround", surround) && !surround) {
            a.channels = 6;
            mark(p);
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("The channel layout the game mixes for: Stereo, or 5.1 surround (front left and right, centre, LFE, rear left and right). Windows "
        "downmixes 5.1 to stereo on a stereo device, so pick 5.1 only for a 5.1 or 7.1 output (Windows sound settings: Configure speakers). "
        "Takes effect at the next start. [audio] channels in the ini wins over this.");
    if (ImGui::Button("Refresh the device list")) load_audio_devices(p);
    ImGui::SameLine();
    ImGui::TextDisabled("%d devices", static_cast<int>(p.audio_devices.size()));
    if (!is_default && std::find(p.audio_devices.begin(), p.audio_devices.end(), a.device) == p.audio_devices.end())
        ImGui::TextColored(kOrange, "'%s' is not connected: the system default is used.", a.device.c_str());
}

void display_page(UiHost& host, const LiveStatus& live, Page& p) {
    window_group(live, p);
    framerate_group(host, live, p);
    resolution_group(p);
    upscale_group(p);
    latency_group(p);

    ImGui::SeparatorText("Live");
    ImGui::Text("Window: %s, present mode: %s", cstr(live.window_mode), cstr(live.present_mode));
    ImGui::Text("Display %.2f Hz, window %d x %d, %.1f fps", live.refresh_hz, live.width, live.height, live.fps);
}

// ---- Bindings -----------------------------------------------------------------------------------------------------------------------

std::string input_button_text(const Binding& b, bool waiting) {
    if (waiting) return "press a key...";
    if (b.in.kind == InKind::None) return "<none>";
    const std::string name = input_name(b.in);
    return name.empty() ? "?" : name;
}

void bindings_tab(UiHost& host, Page& p) {
    if (ImGui::BeginCombo("Context", ctx_name(p.ctx))) {
        for (int c = 0; c < kCtxCount; ++c) {
            const Ctx ctx = static_cast<Ctx>(c);
            if (ImGui::Selectable(ctx_name(ctx), ctx == p.ctx) && ctx != p.ctx) {
                stop_rebind(host, p);
                p.ctx = ctx;
            }
        }
        ImGui::EndCombo();
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    int remove = -1;
    if (ImGui::BeginTable("bindings", 5, kFlags, ImVec2(0.0f, px(320.0f)))) {
        ImGui::TableSetupColumn("Input");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, px(110.0f));
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, px(90.0f));
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, px(32.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
            Binding& b = p.edit.bindings[i];
            if (b.ctx != p.ctx) continue;
            const int row = static_cast<int>(i);
            ImGui::PushID(row);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool waiting = p.rebind == row;
            const std::vector<int> clash = conflicts_of(p, row);
            const std::string label = input_button_text(b, waiting) + "##in";
            if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, kPressed);
            else if (!clash.empty()) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.38f, 0.10f, 1.0f));
            const bool clicked = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f));
            if (waiting || !clash.empty()) ImGui::PopStyleColor();
            if (!clash.empty() && ImGui::IsItemHovered()) {
                std::string who;
                for (int c : clash) who += std::string(who.empty() ? "" : ", ") + act_label(p.edit.bindings[static_cast<std::size_t>(c)].act);
                ImGui::SetItemTooltip("Also bound here to: %s", who.c_str());
            }
            if (clicked) {
                stop_fps_capture(host, p);
                set_notice(p, "", false);
                p.rebind = row;
                host.begin_capture();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##act", act_label(b.act))) {
                for (int a = 0; a < kActCount; ++a) {
                    const Act act = static_cast<Act>(a);
                    if (ImGui::Selectable(act_label(act), act == b.act) && act != b.act) {
                        b.act = act;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##trig", trig_name(b.trig))) {
                for (Trig t : kTrigChoices) {
                    if (ImGui::Selectable(trig_name(t), t == b.trig) && t != b.trig) {
                        b.trig = t;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            const bool timed = b.trig == Trig::Tap || b.trig == Trig::Hold || b.trig == Trig::DoubleTap;
            ImGui::BeginDisabled(!timed);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (int_field("##ms", &b.ms, 0, 5000, 0, 0)) mark(p);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x")) remove = row;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (remove >= 0) {
        if (p.rebind == remove) stop_rebind(host, p);
        else if (p.rebind > remove) --p.rebind;
        p.edit.bindings.erase(p.edit.bindings.begin() + remove);
        mark(p);
    }

    if (ImGui::Button("Add binding")) {
        Binding b;
        b.in = InputId{};
        b.ctx = p.ctx;
        b.act = Act::Primary;
        b.trig = Trig::Press;
        b.ms = 0;
        p.edit.bindings.push_back(b);
        stop_fps_capture(host, p);
        set_notice(p, "", false);
        p.rebind = static_cast<int>(p.edit.bindings.size()) - 1;
        host.begin_capture();
        mark(p);
    }
    // Inputs used twice in one context with the same trigger (their rows are orange): both actions happen when it is pressed.
    int shown = 0;
    for (std::size_t i = 0; i < p.edit.bindings.size() && shown < 6; ++i) {
        const Binding& b = p.edit.bindings[i];
        if (b.ctx != p.ctx) continue;
        for (int c : conflicts_of(p, static_cast<int>(i))) {
            if (c < static_cast<int>(i)) continue;   // each pair once
            ImGui::TextColored(kOrange, "%s is bound to %s and to %s", input_name(b.in).c_str(), act_label(b.act),
                               act_label(p.edit.bindings[static_cast<std::size_t>(c)].act));
            if (++shown >= 6) break;
        }
    }
    ImGui::TextWrapped("Click an input and press the key or mouse button (or turn the wheel) to bind it, Escape cancels. "
                       "Bindings of a context win over the 'all' layer for the same input. Action 'none' switches an input off in that context.");
}

// ---- Switch layout ------------------------------------------------------------------------------------------------------------------

void switch_tab(Page& p) {
    // The cycling actions by the game's names (mpr: beam_next and beam_prev ... beam_1 to beam_4).
    const std::string help = std::string("What each action presses on the Switch pad: buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK "
                                         "and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT, joined by +. ") +
                             act_info(Act::SetANext).name + " and " + act_info(Act::SetAPrev).name + " have no fixed output (they pick " +
                             act_info(Act::SetA1).name + " to " + act_info(Act::SetA4).name + ").";
    ImGui::TextWrapped("%s", help.c_str());
    if (ImGui::BeginTable("switch_out", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
        ImGui::TableSetupColumn("Output");
        ImGui::TableHeadersRow();
        for (int a = 1; a < kActCount; ++a) {
            const bool bad = !p.out_err[a].empty();
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(act_info(static_cast<Act>(a)).name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bad) {
                ImGui::PushStyleColor(ImGuiCol_Border, kRed);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::InputText("##out", p.out_text[a], sizeof p.out_text[a])) {
                Output o;
                std::string err;
                if (parse_output(p.out_text[a], &o, &err)) {
                    p.edit.out[a] = o;
                    p.out_err[a].clear();
                    mark(p);
                } else {
                    p.out_err[a] = err;   // the previous valid output stays in the working copy
                }
            }
            if (bad) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            if (!p.out_err[a].empty()) ImGui::TextColored(kRed, "%s", p.out_err[a].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset this table to the defaults")) {
        for (int a = 1; a < kActCount; ++a) {
            Output o;
            if (parse_output(act_info(static_cast<Act>(a)).default_out, &o)) p.edit.out[a] = o;
        }
        refresh_out_text(p);
        mark(p);
    }
}

// ---- Status -------------------------------------------------------------------------------------------------------------------------

// Live view only: nothing here touches the working copy.
void status_tab(const LiveStatus& live) {
    ImGui::Text("Context: %s", ctx_name(live.ctx));
    ImGui::Text("Aim: %s", cstr(live.aim));
    ImGui::Text("Mouse captured: %s", yes_no(live.mouse_captured));
    ImGui::Text("Pitch: %.1f degrees", live.pitch_deg);
    if (live.input_calls == 0) ImGui::TextDisabled("ProcessInput calls: 0 (the game has not reached gameplay yet)");
    else ImGui::Text("ProcessInput calls: %llu", static_cast<unsigned long long>(live.input_calls));

    ImGui::SeparatorText("Switch pad output");
    for (int i = 0; i < kSwitchButtonCount; ++i) {
        const bool on = (live.out.buttons & hid::btn(kSwitchButtons[i].bit)) != 0;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kPressed : kReleased);
        ImGui::Button(kSwitchButtons[i].name, ImVec2(px(56.0f), 0.0f));
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % 8 != 7) ImGui::SameLine();
    }
    ImGui::Text("LS (%5.2f, %5.2f)   RS (%5.2f, %5.2f)", live.out.lx, live.out.ly, live.out.rx, live.out.ry);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro: %s    Rumble: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Controls and Game --------------------------------------------------------------------------------------------------------------

// The Controls page: the button prompt style on top, then mouse, keyboard bindings, gamepad, the Switch layout and the live status.
void controls_page(UiHost& host, const LiveStatus& live, Page& p) {
    prompts_row(live, p);
    ImGui::Spacing();
    if (ImGui::BeginTabBar("controls_tabs")) {
        if (ImGui::BeginTabItem("Mouse")) {
            mouse_tab(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keyboard and mouse buttons")) {
            bindings_tab(host, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gamepad")) {
            pad_tab(live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Switch layout")) {
            if (!p.out_tab_open) refresh_out_text(p);
            p.out_tab_open = true;
            switch_tab(p);
            ImGui::EndTabItem();
        } else {
            p.out_tab_open = false;
        }
        if (ImGui::BeginTabItem("Status")) {
            status_tab(live);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace

void draw_settings_page(UiHost& host) {
    static Page page;
    Page& p = page;
    if (!p.loaded) {
        p.edit = host.settings();
        p.edit.fps = host.settings().fps;
        refresh_out_text(p);
        p.loaded = true;
    }
    poll_capture(host, p);
    const LiveStatus live = host.status();

    if (host.capturing()) ImGui::TextColored(kYellow, "Press the key or mouse button for the binding, Escape cancels");
    else if (!p.notice.empty()) ImGui::TextColored(p.notice_warn ? kOrange : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", p.notice.c_str());
    ImGui::TextDisabled("Settings file: %s", host.settings_path().c_str());
    if (p.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kOrange, "unsaved changes");
    }
    if (ImGui::Button("Apply and save")) apply_now(host, p);
    tip("Applies the changes to the running game (items marked 'restart' wait for the next start) and writes them to the settings file.");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) revert_to_saved(host, p);
    tip("Drops the changes made since the last Apply and save.");
    ImGui::SameLine();
    if (ImGui::Button("Factory defaults")) load_factory(host, p);
    tip("Loads the factory settings into the page. Nothing changes until Apply and save.");
    ImGui::Separator();

    ImGui::PushItemWidth(std::max(px(200.0f), ImGui::GetContentRegionAvail().x * 0.42f));
    if (ImGui::BeginTabBar("settings_pages")) {
        if (ImGui::BeginTabItem("Display")) {
            display_page(host, live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graphics")) {
            graphics_page(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            controls_page(host, live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            audio_page(p);
            ImGui::EndTabItem();
        }
        if (const GamePlay* gp = game_play(); gp && gp->tweaks_tab && ImGui::BeginTabItem("Game")) {   // the game's optional looks and start-up tweaks
            if (gp->tweaks_tab(p.edit)) mark(p);
            if (fastboot_ui(p.edit.fastboot)) mark(p);   // start-up and menu tweaks (play_ui_fastboot.cpp)
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            draw_about_page(host.settings_path());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
}

}  // namespace nxrt::play
