// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 platform layer (declarations in include/nxrt/platform.h): window, fullscreen, raw relative mouse, gamepad hot-plug,
// Vulkan surface, and the translation of SDL events into nxrt::input events (include/nxrt/input.h).
#include "nxrt/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/haptic.h"
#include "nxrt/input.h"
#include "nxrt/log.h"

namespace nxrt::platform {
namespace {

constexpr const char* kTag = "platform";
constexpr Uint64 kFullscreenWaitMs = 2000;

// All SDL state lives in this file. Everything runs on the thread that created the window.
bool g_inited = false;
bool g_quit = false;
SDL_Window* g_window = nullptr;

struct OpenPad {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
};
std::vector<OpenPad> g_pads;

input::Event make_event(input::EventType type, uint64_t ts) {
    input::Event e;
    e.type = type;
    e.timestamp_ns = ts;
    return e;
}

bool window_flag(SDL_WindowFlags flag) { return g_window != nullptr && (SDL_GetWindowFlags(g_window) & flag) != 0; }

// SDL reports pointer positions in window coordinates; the input queue wants window pixels.
float pixel_density() {
    const float d = g_window ? SDL_GetWindowPixelDensity(g_window) : 0.f;
    return d > 0.f ? d : 1.f;
}

// Client size in pixels, 0 x 0 while minimised or without a window.
void current_pixel_size(int* w, int* h) {
    int pw = 0, ph = 0;
    if (g_window && !window_flag(SDL_WINDOW_MINIMIZED) && !SDL_GetWindowSizeInPixels(g_window, &pw, &ph)) pw = ph = 0;
    if (w) *w = pw;
    if (h) *h = ph;
}

// Copies a UTF-8 string into a fixed buffer, never splitting a multi-byte character, always NUL terminated.
void copy_utf8(char* dst, size_t cap, const char* src) {
    const size_t len = src ? std::strlen(src) : 0;
    size_t n = len < cap - 1 ? len : cap - 1;
    if (n < len) {
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    }
    if (n) std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// The open pad with this id, or nullptr.
SDL_Gamepad* find_pad(uint32_t id) {
    for (const OpenPad& p : g_pads) {
        if (p.id == id) return p.pad;
    }
    return nullptr;
}

// Face positions in GamepadRaw::face_label order: SOUTH, EAST, WEST, NORTH.
constexpr SDL_GamepadButton kFaceButtons[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                               SDL_GAMEPAD_BUTTON_NORTH};

// Maps the label printed on face position p to a GamepadRaw::face_label code (0 A, 1 B, 2 X, 3 Y). Unknown labels keep
// the position default {0,1,2,3}[p], which equals p.
uint8_t face_code(SDL_GamepadButtonLabel label, int p) {
    switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
            return 0;
        case SDL_GAMEPAD_BUTTON_LABEL_B:
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
            return 1;
        case SDL_GAMEPAD_BUTTON_LABEL_X:
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
            return 2;
        case SDL_GAMEPAD_BUTTON_LABEL_Y:
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
            return 3;
        default:
            return static_cast<uint8_t>(p);
    }
}

void read_face_labels(SDL_Gamepad* pad, uint8_t out[4]) {
    for (int p = 0; p < 4; ++p) {
        out[p] = face_code(SDL_GetGamepadButtonLabel(pad, kFaceButtons[p]), p);
    }
}

// Maps SDL's controller type to the families the prompt feature knows; anything else (standard, unknown, virtual...) is Other.
input::PadKind pad_kind(SDL_Gamepad* pad) {
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return input::PadKind::Xbox;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return input::PadKind::PlayStation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return input::PadKind::Switch;
        default:
            return input::PadKind::Other;
    }
}

const char* pad_kind_name(input::PadKind k) {
    switch (k) {
        case input::PadKind::Xbox: return "Xbox";
        case input::PadKind::PlayStation: return "PlayStation";
        case input::PadKind::Switch: return "Switch";
        default: return "other";
    }
}

// Opens a gamepad that SDL reports as added and fills its info entry.
void gamepad_opened(SDL_JoystickID id, uint64_t ts) {
    for (const OpenPad& p : g_pads) {
        if (p.id == id) return;  // duplicate notification for a pad already open
    }
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        NXLOG_W(kTag, "gamepad %u: open failed: %s", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    g_pads.push_back({id, pad});

    input::GamepadInfo info;
    info.id = id;
    const char* name = SDL_GetGamepadName(pad);
    const char* shown = name ? name : "unknown";
    const size_t n = std::strlen(shown) < sizeof info.name - 1 ? std::strlen(shown) : sizeof info.name - 1;
    std::memcpy(info.name, shown, n);
    const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    info.rumble = props != 0 && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    info.gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    info.kind = pad_kind(pad);
    input::gamepad_set_info(info);

    NXLOG_I(kTag, "gamepad %u added: \"%s\" kind=%s rumble=%d gyro=%d", static_cast<unsigned>(id), info.name,
            pad_kind_name(info.kind), info.rumble ? 1 : 0, info.gyro ? 1 : 0);
    // Motion sensors: enabled when the pad has them, skipped silently when it does not.
    const bool accel_on =
        SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    const bool gyro_on = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    uint8_t labels[4] = {0, 1, 2, 3};
    read_face_labels(pad, labels);
    NXLOG_I(kTag, "gamepad %u sensors: accel=%d gyro=%d labels=%d%d%d%d", static_cast<unsigned>(id), accel_on ? 1 : 0,
            gyro_on ? 1 : 0, labels[0], labels[1], labels[2], labels[3]);

    gfx::haptic::pad_added(static_cast<uint32_t>(id));   // the pad is open now: let the haptics pick their backend

    input::Event e = make_event(input::EventType::GamepadAdded, ts);
    e.u.gamepad.id = id;
    input::queue_push(e);
}

// Closes a gamepad that SDL reports as removed and drops its info entry.
void gamepad_closed(SDL_JoystickID id, uint64_t ts) {
    for (auto it = g_pads.begin(); it != g_pads.end(); ++it) {
        if (it->id != id) continue;
        gfx::haptic::pad_removed(static_cast<uint32_t>(id));   // stop the haptics before the joystick goes away
        SDL_CloseGamepad(it->pad);
        g_pads.erase(it);
        input::gamepad_remove_info(id);
        NXLOG_I(kTag, "gamepad %u removed", static_cast<unsigned>(id));
        input::Event e = make_event(input::EventType::GamepadRemoved, ts);
        e.u.gamepad.id = id;
        input::queue_push(e);
        return;
    }
}

// Turns one SDL event into zero or one nxrt input events and updates the quit flag.
void translate(const SDL_Event& ev) {
    const uint64_t ts = ev.common.timestamp;
    switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            g_quit = true;
            input::queue_push(make_event(input::EventType::Quit, ts));
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            input::Event e = make_event(
                ev.type == SDL_EVENT_KEY_DOWN ? input::EventType::KeyDown : input::EventType::KeyUp, ts);
            e.flags = static_cast<uint8_t>(ev.key.repeat ? input::kFlagRepeat : 0);
            e.mods = ev.key.mod;
            e.u.key.scancode = static_cast<int32_t>(ev.key.scancode);
            e.u.key.keycode = static_cast<int32_t>(ev.key.key);
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_TEXT_INPUT: {
            input::Event e = make_event(input::EventType::Text, ts);
            copy_utf8(e.u.text.utf8, sizeof e.u.text.utf8, ev.text.text);
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            const bool rel = relative_mouse();
            const float d = pixel_density();
            input::Event e = make_event(input::EventType::MouseMotion, ts);
            e.flags = static_cast<uint8_t>(rel ? input::kFlagRelative : 0);
            e.u.motion.dx = ev.motion.xrel;
            e.u.motion.dy = ev.motion.yrel;
            e.u.motion.x = ev.motion.x * d;
            e.u.motion.y = ev.motion.y * d;
            if (rel) input::mouse_delta_add(ev.motion.xrel, ev.motion.yrel);
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const float d = pixel_density();
            input::Event e = make_event(
                ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? input::EventType::MouseButtonDown : input::EventType::MouseButtonUp,
                ts);
            e.mods = SDL_GetModState();
            e.u.button.button = ev.button.button;
            e.u.button.clicks = ev.button.clicks;
            e.u.button.x = ev.button.x * d;
            e.u.button.y = ev.button.y * d;
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            input::Event e = make_event(input::EventType::MouseWheel, ts);
            e.u.wheel.dx = ev.wheel.x;
            e.u.wheel.dy = ev.wheel.y;
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_GAMEPAD_ADDED:
            gamepad_opened(ev.gdevice.which, ts);
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            gamepad_closed(ev.gdevice.which, ts);
            break;

        case SDL_EVENT_AUDIO_DEVICE_ADDED:
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
            // The DualSense haptics ride on its USB audio endpoint: look for it as soon as SDL says it changed.
            gfx::haptic::audio_devices_changed();
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            input::Event e = make_event(ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? input::EventType::GamepadButtonDown
                                                                                 : input::EventType::GamepadButtonUp,
                                        ts);
            e.u.gpbutton.id = ev.gbutton.which;
            e.u.gpbutton.button = ev.gbutton.button;
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            input::Event e = make_event(input::EventType::GamepadAxis, ts);
            e.u.gpaxis.id = ev.gaxis.which;
            e.u.gpaxis.axis = ev.gaxis.axis;
            e.u.gpaxis.value = ev.gaxis.value;
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            input::Event e = make_event(input::EventType::WindowResized, ts);
            e.u.window.w = ev.window.data1;
            e.u.window.h = ev.window.data2;
            input::queue_push(e);
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            input::queue_push(make_event(input::EventType::WindowFocusGained, ts));
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            input::queue_push(make_event(input::EventType::WindowFocusLost, ts));
            break;

        case SDL_EVENT_WINDOW_MINIMIZED:
            input::queue_push(make_event(input::EventType::WindowMinimized, ts));
            break;

        case SDL_EVENT_WINDOW_RESTORED:
            input::queue_push(make_event(input::EventType::WindowRestored, ts));
            break;

        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
            input::Event e = make_event(input::EventType::WindowFullscreenChanged, ts);
            e.flags = static_cast<uint8_t>(ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? input::kFlagFullscreen : 0);
            current_pixel_size(&e.u.window.w, &e.u.window.h);
            input::queue_push(e);
            break;
        }

        default:
            break;
    }
}

}  // namespace

// ---- lifecycle ----------------------------------------------------------------------------------------------------

bool init(const InitOptions& opt) {
    if (g_inited) return true;
    // Hints must be set before SDL_Init. Raw, unaccelerated relative mouse; gamepads keep reporting when the window is
    // in the background.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (opt.gamepads) flags |= SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD;
    if (!SDL_Init(flags)) {
        NXLOG_E(kTag, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    g_inited = true;
    g_quit = false;
    const int v = SDL_GetVersion();
    NXLOG_I(kTag, "SDL %d.%d.%d initialised (gamepads %s)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
            SDL_VERSIONNUM_MICRO(v), opt.gamepads ? "on" : "off");
    return true;
}

void shutdown() {
    if (!g_inited) return;
    gfx::haptic::shutdown();
    destroy_window();
    for (const OpenPad& p : g_pads) {
        SDL_CloseGamepad(p.pad);
        input::gamepad_remove_info(p.id);
    }
    g_pads.clear();
    SDL_Quit();
    g_inited = false;
    NXLOG_I(kTag, "shutdown");
}

bool initialised() { return g_inited; }

// ---- window -----------------------------------------------------------------------------------------------------

bool create_window(const WindowDesc& desc) {
    if (!g_inited && !init()) return false;
    if (g_window) destroy_window();  // one window at a time

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.hidden || config_get_bool("gfx", "hidden", false)) flags |= SDL_WINDOW_HIDDEN;

    g_window = SDL_CreateWindow(desc.title ? desc.title : "nxrt", desc.width, desc.height, flags);
    if (!g_window) {
        NXLOG_E(kTag, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    int pw = 0, ph = 0;
    current_pixel_size(&pw, &ph);
    NXLOG_I(kTag, "window %dx%d, pixels %dx%d, density %.2f%s", desc.width, desc.height, pw, ph,
            static_cast<double>(pixel_density()), (flags & SDL_WINDOW_HIDDEN) ? " (hidden)" : "");
    return true;
}

void destroy_window() {
    if (!g_window) return;
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
}

bool has_window() { return g_window != nullptr; }

void* native_window() { return g_window; }

void* native_hinstance() {
    if (!g_window) return nullptr;
    return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
}

void* native_hwnd() {
    if (!g_window) return nullptr;
    return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void set_window_title(const char* title) {
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
}

void pixel_size(int* w, int* h) { current_pixel_size(w, h); }

bool set_window_size(int w, int h) { return g_window != nullptr && SDL_SetWindowSize(g_window, w, h); }

bool minimized() { return window_flag(SDL_WINDOW_MINIMIZED); }

bool focused() { return window_flag(SDL_WINDOW_INPUT_FOCUS); }

bool visible() { return g_window != nullptr && !window_flag(SDL_WINDOW_HIDDEN); }

// ---- fullscreen ---------------------------------------------------------------------------------------------------

bool set_fullscreen(FullscreenMode mode, int w, int h, float hz) {
    if (!g_window) return false;
    bool ok = false;
    switch (mode) {
        case FullscreenMode::Windowed:
            ok = SDL_SetWindowFullscreen(g_window, false);
            break;
        case FullscreenMode::Borderless:
            // A NULL mode means "desktop resolution, borderless".
            ok = SDL_SetWindowFullscreenMode(g_window, nullptr) && SDL_SetWindowFullscreen(g_window, true);
            break;
        case FullscreenMode::Exclusive: {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
            SDL_DisplayMode chosen{};
            bool found = false;
            if (w == 0 && h == 0) {
                const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
                if (desktop) {
                    chosen = *desktop;
                    found = true;
                }
            } else {
                found = SDL_GetClosestFullscreenDisplayMode(display, w, h, hz, true, &chosen);
            }
            ok = found && SDL_SetWindowFullscreenMode(g_window, &chosen) && SDL_SetWindowFullscreen(g_window, true);
            break;
        }
    }
    if (!ok) {
        NXLOG_W(kTag, "set_fullscreen(%d) refused: %s", static_cast<int>(mode), SDL_GetError());
        return false;
    }

    // Wait until SDL reports the new state, pumping events meanwhile (SDL applies it through the event loop).
    SDL_SyncWindow(g_window);
    const Uint64 deadline = SDL_GetTicks() + kFullscreenWaitMs;
    while (fullscreen_mode() != mode && SDL_GetTicks() < deadline) pump_events(true, 20);

    const bool reached = fullscreen_mode() == mode;
    if (!reached) NXLOG_W(kTag, "set_fullscreen(%d): SDL did not report the new state within %llu ms",
                          static_cast<int>(mode), static_cast<unsigned long long>(kFullscreenWaitMs));
    return reached;
}

FullscreenMode fullscreen_mode() {
    if (!window_flag(SDL_WINDOW_FULLSCREEN)) return FullscreenMode::Windowed;
    return SDL_GetWindowFullscreenMode(g_window) ? FullscreenMode::Exclusive : FullscreenMode::Borderless;
}

float display_refresh_hz() {
    if (!g_window) return 0.f;
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(g_window));
    return m ? m->refresh_rate : 0.f;
}

// ---- displays -----------------------------------------------------------------------------------------------------

namespace {
// The display ids in the system's order; empty when SDL has none. The caller frees nothing.
std::vector<SDL_DisplayID> display_ids() {
    std::vector<SDL_DisplayID> out;
    int n = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
        out.assign(ids, ids + (n > 0 ? n : 0));
        SDL_free(ids);
    }
    return out;
}
}  // namespace

int display_count() { return static_cast<int>(display_ids().size()); }

bool display_name(int number, char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = '\0';
    const std::vector<SDL_DisplayID> ids = display_ids();
    if (number < 1 || number > static_cast<int>(ids.size())) return false;
    const SDL_DisplayID id = ids[static_cast<size_t>(number - 1)];
    const char* name = SDL_GetDisplayName(id);
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id);
    if (m) std::snprintf(out, static_cast<size_t>(cap), "%d: %s (%dx%d @ %.0f Hz)", number, name && *name ? name : "Display", m->w, m->h, static_cast<double>(m->refresh_rate));
    else std::snprintf(out, static_cast<size_t>(cap), "%d: %s", number, name && *name ? name : "Display");
    return true;
}

int window_display() {
    if (!g_window) return 0;
    const SDL_DisplayID cur = SDL_GetDisplayForWindow(g_window);
    const std::vector<SDL_DisplayID> ids = display_ids();
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == cur) return static_cast<int>(i) + 1;
    return 0;
}

bool move_to_display(int number) {
    if (!g_window) return false;
    const std::vector<SDL_DisplayID> ids = display_ids();
    if (number < 1 || number > static_cast<int>(ids.size())) return false;
    const SDL_DisplayID id = ids[static_cast<size_t>(number - 1)];
    if (SDL_GetDisplayForWindow(g_window) == id) return true;
    const FullscreenMode mode = fullscreen_mode();
    if (mode != FullscreenMode::Windowed && !set_fullscreen(FullscreenMode::Windowed)) return false;
    const bool ok = SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(id), SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    if (!ok) NXLOG_W(kTag, "move_to_display(%d) failed: %s", number, SDL_GetError());
    SDL_SyncWindow(g_window);
    if (mode != FullscreenMode::Windowed) set_fullscreen(mode);
    return ok;
}

// ---- relative mouse -----------------------------------------------------------------------------------------------

bool set_relative_mouse(bool on) {
    if (!g_window) return false;
    if (!SDL_SetWindowRelativeMouseMode(g_window, on)) {
        NXLOG_W(kTag, "set_relative_mouse(%d) failed: %s", on ? 1 : 0, SDL_GetError());
        return false;
    }
    return true;
}

bool relative_mouse() { return g_window != nullptr && SDL_GetWindowRelativeMouseMode(g_window); }

// The OS cursor of the window (menus draw it; relative mode hides it by itself). Cheap when nothing changes.
void set_cursor_visible(bool on) {
    if (!g_window) return;
    if (on == SDL_CursorVisible()) return;
    if (on) SDL_ShowCursor();
    else SDL_HideCursor();
}

void warp_mouse(float x, float y) {
    if (!g_window) return;
    const float d = pixel_density();
    SDL_WarpMouseInWindow(g_window, d > 0 ? x / d : x, d > 0 ? y / d : y);
}

void set_text_input(bool on) {
    if (!g_window || SDL_TextInputActive(g_window) == on) return;
    if (!(on ? SDL_StartTextInput(g_window) : SDL_StopTextInput(g_window))) NXLOG_W(kTag, "set_text_input(%d) failed: %s", on ? 1 : 0, SDL_GetError());
}

bool relative_mouse_is_raw() {
    const char* v = SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE);
    return v == nullptr || std::strcmp(v, "0") == 0;
}

// ---- events -------------------------------------------------------------------------------------------------------

int pump_events(bool wait, int timeout_ms) {
    if (!g_inited) return 0;
    SDL_Event ev;
    int handled = 0;
    bool have = false;
    if (wait) {
        have = timeout_ms < 0 ? SDL_WaitEvent(&ev) : SDL_WaitEventTimeout(&ev, timeout_ms);
    } else {
        have = SDL_PollEvent(&ev);
    }
    while (have) {
        ++handled;
        translate(ev);
        have = SDL_PollEvent(&ev);
    }
    return handled;
}

bool quit_requested() { return g_quit; }

void clear_quit() { g_quit = false; }

void request_quit() { g_quit = true; }

uint64_t ticks_ns() { return SDL_GetTicksNS(); }

// ---- gamepad polling, sensors and rumble ------------------------------------------------------------------------------

bool gamepad_poll(uint32_t id, GamepadRaw* out) {
    SDL_Gamepad* pad = find_pad(id);
    if (!pad || !out) return false;
    static_assert(SDL_GAMEPAD_AXIS_COUNT == 6, "GamepadRaw::axes holds the six SDL axes");
    GamepadRaw s;
    for (int n = 0; n < SDL_GAMEPAD_BUTTON_COUNT && n < 32; ++n) {
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(n))) s.buttons |= uint32_t{1} << n;
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
        s.axes[i] = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
    }
    read_face_labels(pad, s.face_label);
    *out = s;
    return true;
}

bool gamepad_sensors(uint32_t id, float accel[3], float gyro[3]) {
    SDL_Gamepad* pad = find_pad(id);
    if (!pad || !accel || !gyro) return false;
    const bool present = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_GamepadSensorEnabled(pad, SDL_SENSOR_ACCEL) &&
                         SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO);
    if (!present) return false;
    return SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3) &&
           SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3);
}

bool gamepad_rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t duration_ms) {
    SDL_Gamepad* pad = find_pad(id);
    if (!pad) return false;
    return SDL_RumbleGamepad(pad, low, high, duration_ms);
}

// ---- Vulkan -------------------------------------------------------------------------------------------------------

const char* const* vulkan_instance_extensions(uint32_t* count) {
    if (count) *count = 0;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        NXLOG_E(kTag, "SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        return nullptr;
    }
    Uint32 n = 0;
    const char* const* list = SDL_Vulkan_GetInstanceExtensions(&n);
    if (count) *count = n;
    return list;
}

bool create_vulkan_surface(void* vk_instance, uint64_t* surface_out) {
    if (!g_window || !vk_instance || !surface_out) return false;
    VkSurfaceKHR surface = nullptr;
    if (!SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(vk_instance), nullptr, &surface)) {
        NXLOG_E(kTag, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    *surface_out = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(surface));
    return true;
}

void destroy_vulkan_surface(void* vk_instance, uint64_t surface) {
    if (!vk_instance || !surface) return;
    SDL_Vulkan_DestroySurface(static_cast<VkInstance>(vk_instance),
                              reinterpret_cast<VkSurfaceKHR>(static_cast<uintptr_t>(surface)), nullptr);
}

}  // namespace nxrt::platform
