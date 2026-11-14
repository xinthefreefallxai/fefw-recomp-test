// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the glue. See include/nxrt/play.h for the start-up order and docs/S8.md for the design.
//
// Threads:
//   main (the guest main thread, which created the window)  the Update hook (frame boundary: event pump, input, settings changes) and the
//                                                            ProcessInput hook (raw mouse aim) run here
//   presenting thread (the game's render thread)             FrameLimiter::begin_frame (the pacing wait, and the pre-frame hook that changes the
//                                                            present mode), the ImGui overlay and the settings page
//   hid sampling thread                                      PlaySource::poll (200 Hz): reads the pad snapshot and asks the binding engine for the
//                                                            keyboard and mouse contribution at that instant
#include "nxrt/pgo.h"
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "nxrt/play.h"

#include <windows.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/ftrace.h"
#include "nxrt/game.h"
#include "play_display.h"
#include "nxrt/kernel.h"
#include "nxrt/svc.h"
#include "nxrt/config.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/services_api.h"
#include "nxrt/haptic.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "nxrt/runtime.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"
#include "menu_mouse.h"
#include "play_engine.h"
#include "play_game.h"
#include "play_selftest.h"
#include "play_types.h"
#include "play_vpad.h"
#include "play_ui.h"
#include "prompts.h"
#include "pad_map.h"
#include "pad_orient.h"

namespace nxrt::play {
namespace {
// Keep-alive pump: the window's events are normally pumped at the frame boundary. While the game boots or loads it can go ten seconds and
// more without one, and Windows then calls the window "Not Responding". busy_pump runs before every SVC (svc_set_busy_hook); on the
// thread that owns the window it pumps the events when the last pump is 100 ms old. Elsewhere it costs a thread-id compare.
std::atomic<DWORD> g_pump_tid{0};
std::atomic<uint64_t> g_last_pump_ms{0};
void keyboard_service();
void busy_pump() {
    if (GetCurrentThreadId() != g_pump_tid.load(std::memory_order_relaxed)) return;
    keyboard_service();
    const uint64_t t = SDL_GetTicks();
    if (t - g_last_pump_ms.load(std::memory_order_relaxed) < 100) return;
    g_last_pump_ms.store(t, std::memory_order_relaxed);
    platform::pump_events();
}
}  // namespace

namespace {

constexpr const char* kTag = "play";
constexpr uint64_t kMs = 1000000ull;
namespace haptic = nxrt::gfx::haptic;

// SDL mouse button (1 left, 2 middle, 3 right, 4 X1, 5 X2) to nn::hid::MouseButton (Left 0, Right 1, Middle 2, Forward 3, Back 4).
uint32_t jc2_button_bit(uint8_t b) {
    switch (b) {
        case 1: return 1u << 0;
        case 3: return 1u << 1;
        case 2: return 1u << 2;
        case 5: return 1u << 3;
        case 4: return 1u << 4;
        default: return 0;
    }
}

struct PadFrame {
    uint64_t buttons = 0;
    hid::StickValue left, right;
    hid::SixAxisSample six;
};

struct Touch {
    bool down = false;
    uint32_t x = 0, y = 0;     // on the 1280 x 720 touch screen
};

struct G;

// ---- the hid input source ------------------------------------------------------------------------------------------------------------
class PlaySource : public hid::InputSource {
public:
    explicit PlaySource(Engine* e) : engine_(e) {}
    void poll(hid::InputSnapshot& out) override {
        const EngineOut kb = engine_->output(FrameLimiter::now_ns());
        PadFrame pf;
        Touch t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pf = pad_;
            t = touch_;
        }
        hid::NpadInput& n = out.npad[0];
        n.connected = true;
        n.style = hid::NpadStyle::FullKey;
        n.buttons = pf.buttons | kb.buttons | jc2_buttons_.load(std::memory_order_relaxed);
        n.left = combine(pf.left, kb.lx, kb.ly);
        n.right = combine(pf.right, kb.rx, kb.ry);
        n.six_axis = pf.six;
        {   // the pad's orientation (the game's pointer scheme reads it): integrated from the gyro, held level by the accelerometer
            const uint64_t t = FrameLimiter::now_ns();
            const double dt = last_orient_ns_ ? static_cast<double>(t - last_orient_ns_) / 1e9 : 0.0;
            last_orient_ns_ = t;
            orient_.step(pf.six.gyro, pf.six.accel, dt);
            std::memcpy(n.six_axis.direction, orient_.direction, sizeof orient_.direction);
            std::memcpy(n.six_axis.angle, orient_.angle, sizeof orient_.angle);
        }
        right_y_.store(n.right.y, std::memory_order_relaxed);
        right_x_f_.store(static_cast<float>(n.right.x) / 32767.f, std::memory_order_relaxed);
        right_y_f_.store(static_cast<float>(n.right.y) / 32767.f, std::memory_order_relaxed);
        gyro_pitch_.store(std::fabs(pf.six.gyro[0]), std::memory_order_relaxed);
        n.six_axis.gyro[0] *= look_scale_.load(std::memory_order_relaxed);      // the game integrates the pitch rate per call: see play_lookrate.h
        out.touch.count = 0;
        if (t.down) {
            out.touch.count = 1;
            hid::TouchFinger& f = out.touch.fingers[0];
            f = hid::TouchFinger{};
            f.id = 0;
            f.x = t.x;
            f.y = t.y;
        }
    }
    void set_pad(const PadFrame& p) {
        std::lock_guard<std::mutex> lk(mu_);
        pad_ = p;
    }
    // Is a pad looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis)? Read by the aim
    // on the main thread; the values are the ones the game gets.
    bool pad_look(bool gyro_enabled) const {
        return std::abs(right_y_.load(std::memory_order_relaxed)) > 650 || (gyro_enabled && gyro_pitch_.load(std::memory_order_relaxed) > 0.02f);
    }
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up: the alternate body's camera (mpr: the morph ball camera) orbits with it.
    void right_stick(float* x, float* y) const {
        *x = right_x_f_.load(std::memory_order_relaxed);
        *y = right_y_f_.load(std::memory_order_relaxed);
    }
    void set_look_scale(float k) { look_scale_.store(k, std::memory_order_relaxed); }      // the gyro's pitch rate times k (step * 60), for the game's per-call pitch
    // The Joy-Con 2 mouse's own buttons (its R / ZR under the fingers), which reach the game as npad buttons.
    void set_jc2_buttons(uint64_t b) { jc2_buttons_.store(b, std::memory_order_relaxed); }
    void set_touch(const Touch& t) {
        std::lock_guard<std::mutex> lk(mu_);
        touch_ = t;
    }

private:
    static hid::StickValue combine(hid::StickValue pad, float kx, float ky) {
        double x = pad.x + static_cast<double>(kx) * 32767.0, y = pad.y + static_cast<double>(ky) * 32767.0;
        const double len = std::sqrt(x * x + y * y);
        if (len > 32767.0) { x *= 32767.0 / len; y *= 32767.0 / len; }
        hid::StickValue r;
        r.x = static_cast<int32_t>(std::lround(x));
        r.y = static_cast<int32_t>(std::lround(y));
        return r;
    }
    Engine* engine_;
    PadOrient orient_;
    uint64_t last_orient_ns_ = 0;
    std::atomic<int32_t> right_y_{0};
    std::atomic<float> right_x_f_{0.f}, right_y_f_{0.f};
    std::atomic<float> look_scale_{1.f};
    std::atomic<float> gyro_pitch_{0.f};
    std::atomic<uint64_t> jc2_buttons_{0};
    std::mutex mu_;
    PadFrame pad_;
    Touch touch_;
};

// ---- the state ------------------------------------------------------------------------------------------------------------------------
struct G {
    // settings
    std::mutex mu;                       // guards settings (written on the main thread, copied by the page on the presenting thread) and pending
    Settings settings;
    Settings pending;
    bool have_pending = false;
    bool dirty = false;                  // settings changed in the game (Alt+Enter): written at stop
    std::string path;
    bool loaded = false, started = false, load_ok = true;

    FrameLimiter limiter;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<PlaySource> source_owner;
    std::shared_ptr<hid::InputSource> source;
    std::unique_ptr<gfx::Overlay> overlay;
    gfx::Renderer* renderer = nullptr;
    gfx::Device* device = nullptr;
    bool window = false;

    std::thread::id pump_thread;
    uint64_t base = 0;                   // set by load: [play] game_base, default the game's Info::main_base
    uint32_t hook_update = 0;            // the frame boundary (the game's GamePlay::frame_update)
    bool capture_mouse = true, quit_on_close = true, log_status = false;
    bool pointer_draw = false;         // [play] pointer_draw: the menu pointer is also drawn into the frame (screenshots show it); a test aid
    bool late_sample = true;           // pump the window's events again right before the mouse counts are taken (S11: the counts are as new as ProcessInput)
    double pitch_limit_deg = 0;

    // frame state (main thread)
    uint64_t update_seq = 0, aim_seq = 0;
    std::atomic<uint64_t> last_pi_ns{0};
    std::atomic<uint64_t> last_pi_frame{0};       // value of `frames` at the last ProcessInput (the game is playing while it keeps being called)
    uint64_t map_acts_seen = 0;
    bool map_hint = false;
    bool overlay_was_visible = false;
    bool was_in_game = false;
    float mouse_x = 0, mouse_y = 0;
    bool mouse1 = false;
    uint32_t jc2_buttons = 0;            // host mouse buttons as nn::hid::MouseButton bits, for the Joy-Con 2 mouse (hid jc2_mouse.h)
    uint32_t jc2_presses = 0;            // buttons pressed since the last feed
    float jc2_wheel_x = 0, jc2_wheel_y = 0;   // wheel notches since the last feed
    float jc2_dx = 0, jc2_dy = 0;        // mouse motion since the last feed, raw counts (relative mode) or window pixels
    bool jc2_captured = false;           // play captured the mouse for the Joy-Con 2 mouse (not for a camera)
    uint64_t jc2_click_until[2] = {};    // a click shorter than a frame still reaches the game: left (R), right (ZR) held until then
    uint64_t last_status_ns = 0;
    bool warned_thread = false;

    // button prompts: the device the player used last (Auto style) and a counter that changes with the bindings
    LastDevice last_dev = LastDevice::None;
    int last_pad_kind = 0;
    uint64_t settings_epoch = 1;

    // pads
    uint64_t last_rumble_ns = 0;
    uint16_t rumble_lo = 0, rumble_hi = 0;

    // present mode requested by the page, applied on the presenting thread
    std::atomic<int> want_present{-1};

    // vibration from the game (hid thread) -> pads (main thread)
    std::atomic<float> vib[2][2] = {};   // [hand][low, high] amplitude

    // rebinding
    std::atomic<int> capture_state{0};   // 0 idle, 1 waiting for an input
    std::mutex cap_mu;
    bool cap_have = false;
    InputId cap_id;

    // live status
    mutable std::mutex st_mu;
    LiveStatus live;
    uint64_t fps_last_frames = 0, fps_last_ns = 0;
    double fps = 0;

    SelfTest selftest;

    // counters
    std::atomic<uint64_t> frames{0}, pi_calls{0}, aim_frames{0}, events{0}, counts{0};
    std::atomic<bool> thread_mismatch{false};
};

SelfTestHost* selftest_host();      // defined after the settings page host

G& g() {
    static G inst;
    return inst;
}

uint64_t now_ns() { return FrameLimiter::now_ns(); }

// vsync off: immediate (tearing, lowest latency; mailbox where immediate is missing). vsync on: FIFO, except with [display] fps_limit = 0
// (uncapped): mailbox, so the game renders as fast as it can and the display still shows whole frames.
gfx::PresentMode present_mode_for(const DisplaySettings& d) {
    if (!d.vsync) return gfx::PresentMode::Immediate;
    return d.fps_cap == 0 ? gfx::PresentMode::Mailbox : gfx::PresentMode::Fifo;
}

const char* window_mode_name(platform::FullscreenMode m) {
    switch (m) {
        case platform::FullscreenMode::Windowed: return "windowed";
        case platform::FullscreenMode::Borderless: return "borderless";
        case platform::FullscreenMode::Exclusive: return "exclusive";
    }
    return "?";
}

// Master volume (live). The mute while the window has no focus is set every frame (update_hook) and on focus changes.
void apply_audio(const AudioSettings& a) { audio::set_master_volume(static_cast<float>(a.volume)); }

void apply_unfocused_mute() {
    G& st = g();
    audio::set_muted(st.window && st.settings.audio.mute_unfocused && !platform::focused());
}

// The display's refresh rate for the frame cap. NXRT_FORCE_REFRESH_HZ=<n> (a test hook: check the 60 Hz paths on a faster display) replaces it.
float cap_refresh_hz(bool window) {
    static const float forced = [] {
        const char* e = std::getenv("NXRT_FORCE_REFRESH_HZ");
        return e ? static_cast<float>(std::atof(e)) : 0.f;
    }();
    if (forced > 0.f) return forced;
    return window ? platform::display_refresh_hz() : 0.f;
}

void apply_frame_cap(const Settings& s) {
    G& st = g();
    double cap = s.display.fps_cap;
    const double floor_fps = frame_cap_floor();
    static double logged_cap = -2, logged_floor = -2;
    static float logged_hz = -2;
    const bool changed = cap != logged_cap || floor_fps != logged_floor;
    if (cap < 0) {
        // fps_limit = auto: the display's refresh rate (no limit while it is unknown, e.g. headless runs)
        const float hz = cap_refresh_hz(st.window);
        cap = hz > 0 ? static_cast<double>(hz) : 0;
        if (floor_fps > 0 && cap > 0 && cap < floor_fps && (changed || hz != logged_hz))
            NXLOG_W(kTag, "the game runs at %.0f fps but [display] fps_limit = auto follows the display's %.0f Hz: at most %.0f fps are presented%s",
                    floor_fps, cap, cap, s.display.vsync ? " (vsync is on too)" : "; a faster display or a numeric fps_limit lifts it");
        logged_hz = hz;
    } else if (floor_fps > 0 && cap > 0 && cap < floor_fps) {
        if (changed)
            NXLOG_I(kTag, "[display] fps_limit = %.0f is below the game's %.0f fps: the limit is raised to %.0f for this run (controls.ini keeps %.0f)",
                    cap, floor_fps, floor_fps, cap);
        cap = floor_fps;
    }
    logged_cap = s.display.fps_cap;
    logged_floor = floor_fps;
    if (s.display.vsync && s.display.fps_cap == 0) {
        cap = 0;     // uncapped with vsync: mailbox presents, no limiter
    } else if (s.display.vsync) {
        // The display paces; a cap only matters when it is below the refresh rate. With no cap below the refresh rate, the frame limiter still keeps just
        // under it (S21, [latency] vsync_cap): the game otherwise renders as fast as the swapchain takes frames, the queue behind the display
        // fills (three images) and every picture is shown one to two frames after it was simulated; just under the refresh rate the queue stays empty.
        const float hz = cap_refresh_hz(st.window);
        static const bool vsync_cap = config_get_bool("latency", "vsync_cap", true);
        static double warned_hz = -1;
        if (floor_fps > 0 && hz > 0 && hz < floor_fps && warned_hz != hz) {
            warned_hz = hz;
            NXLOG_W(kTag, "the game runs at %.0f fps but vsync is on and the display refreshes at %.0f Hz: at most %.0f fps are shown (turn vsync off, "
                    "or use a faster display)", floor_fps, static_cast<double>(hz), static_cast<double>(hz));
        }
        if (cap <= 0 || (hz > 0 && cap >= hz)) cap = (vsync_cap && hz > 0) ? static_cast<double>(hz) * 0.9995 : 0;
    }
    st.limiter.set_target_fps(cap);
    extras::lowlat_note_cap(cap);     // Reflex / Anti-Lag pace to the same cap (remembered for a renderer that is created later)
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------------
void pre_frame(void*) {
    // On the presenting thread, before the frame: the one place a present-mode change cannot collide with a frame in flight.
    G& st = g();
    const int want = st.want_present.exchange(-1);
    if (st.renderer && want >= 0) st.renderer->set_present_mode(static_cast<gfx::PresentMode>(want));
    if (st.overlay) st.overlay->apply_present_pending();
}

// The `mouse <dx> <dy>` test step: the counts go to the camera's accumulator as before and arrive as a relative motion event, as a real
// mouse's do (the Joy-Con 2 mouse takes its motion from the events).
void test_mouse_sink(double dx, double dy) {
    input::mouse_delta_add(dx, dy);
    input::Event m;
    m.type = input::EventType::MouseMotion;
    m.flags = input::kFlagRelative;
    m.u.motion.dx = static_cast<float>(dx);
    m.u.motion.dy = static_cast<float>(dy);
    input::queue_push(m);
}

bool key_sink(const char* name, bool down) {
    InputId id;
    static float s_mx = 0, s_my = 0;     // the last scripted pointer position: a scripted click lands there, as a real one would
    // Scripted pointer: `keydown mpos <x> <y>` puts the pointer at window pixel (x, y) the way a real motion event does (S28 tests).
    if (name && std::strncmp(name, "mpos ", 5) == 0) {
        if (!down) return true;
        float px = 0, py = 0;
        if (std::sscanf(name + 5, "%f %f", &px, &py) != 2) return false;
        s_mx = px;
        s_my = py;
        input::Event m;
        m.type = input::EventType::MouseMotion;
        m.u.motion.x = px;
        m.u.motion.y = py;
        input::queue_push(m);
        return true;
    }
    // Scripted virtual gamepad (S35 tests): `keydown vpad attach xbox`, `keydown vpad axis ry 0.8`, `keydown vpad button south 1`, `keydown vpad gyro 0 1 0` ...
    if (name && std::strncmp(name, "vpad ", 5) == 0) return !down || vpad_command(name + 5);
    if (!name || !parse_input(name, &id)) return false;
    input::Event e;
    e.timestamp_ns = 0;
    switch (id.kind) {
        case InKind::Key:
            e.type = down ? input::EventType::KeyDown : input::EventType::KeyUp;
            e.u.key.scancode = id.code;
            e.u.key.keycode = static_cast<int32_t>(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id.code), SDL_KMOD_NONE, false));
            break;
        case InKind::Mouse:
            e.type = down ? input::EventType::MouseButtonDown : input::EventType::MouseButtonUp;
            e.u.button.button = static_cast<uint8_t>(id.code);
            e.u.button.clicks = 1;
            e.u.button.x = s_mx;
            e.u.button.y = s_my;
            break;
        case InKind::Wheel:
            if (!down) return true;      // a notch has no release
            e.type = input::EventType::MouseWheel;
            e.u.wheel.dy = static_cast<float>(id.code);
            break;
        default: return false;
    }
    input::queue_push(e);
    return true;
}

// @shot in a script: the renderer writes the next presented frame as a PNG; the call returns when the file exists (or after 5 s).
bool screenshot_provider(const char* path, void*) {
    G& st = g();
    if (!st.renderer || !path) return false;
    DeleteFileA(path);
    if (std::strstr(path, "nvntrace_")) nvn::backend_trace_next_frame();   // `@shot nvntrace_<name>`: also trace the frame that is captured
    // The caller is the game's thread at a frame boundary, and the capture happens at the render thread's next end of frame, which
    // needs the game's next frame: waiting here for the file held every shot for the 5 s timeout. Wait only for an earlier request to
    // be taken (one capture path at a time), then return; the file is written on a worker and the test runner waits for it at exit.
    for (int i = 0; i < 200 && st.renderer->capture_pending(); ++i) Sleep(5);
    st.renderer->request_screenshot(path);
    return true;
}

void vib_sink(uint32_t slot, int hand, const hid::VibrationValue& v, void*) {
    if (slot != 0 || hand < 0 || hand > 1) return;
    G& st = g();
    st.vib[hand][0].store(v.amp_low);
    st.vib[hand][1].store(v.amp_high);
    // The haptic backends need the frequencies too: the standard rumble below keeps only the amplitudes.
    haptic::push(slot, hand, v.amp_low, v.freq_low, v.amp_high, v.freq_high);
}

}  // namespace

[[noreturn]] void exit_process(int code, const char* verdict, const std::string& message) {
    G& st = g();
    NXLOG_I(kTag, "ending the process (%s): %s", verdict, message.c_str());
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        if (!save_settings(st.path, st.settings, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
        st.dirty = false;
    }
    nvn::backend_flush_caches();                    // the pipeline cache; this path does not shut the back end down
    nxrt::log_flush();
    nxrt::profile_flush();                          // profile-guided builds: the counters (TerminateProcess skips the profile runtime's atexit)
    if (tt::enabled() && !tt::finished()) tt::end_run(code, verdict, message);     // writes result.json, then terminates
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) Sleep(1000);
}

namespace {

[[noreturn]] void quit_now(const char* why) { exit_process(0, "pass", why); }

void set_window_mode(const std::string& mode) {
    G& st = g();
    if (!st.window) return;
    const platform::FullscreenMode want = mode == "borderless" ? platform::FullscreenMode::Borderless
                                         : mode == "exclusive" ? platform::FullscreenMode::Exclusive
                                                               : platform::FullscreenMode::Windowed;
    if (platform::fullscreen_mode() == want) return;
    if (!platform::set_fullscreen(want)) {
        NXLOG_W(kTag, "window mode '%s' was refused", mode.c_str());
        // Exclusive fullscreen needs the display to accept a mode change (it does not on a remote or locked session, or with some drivers).
        // The next best thing is borderless at the desktop mode, which always works; the setting stays as the player chose it.
        if (want == platform::FullscreenMode::Exclusive && platform::fullscreen_mode() == platform::FullscreenMode::Windowed) {
            if (platform::set_fullscreen(platform::FullscreenMode::Borderless)) NXLOG_W(kTag, "using borderless fullscreen instead");
            else NXLOG_W(kTag, "borderless fullscreen was refused too; staying windowed");
        }
    }
    st.limiter.reset_schedule();
}

void toggle_fullscreen() {
    G& st = g();
    if (!st.window) return;
    std::string target;
    if (platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
        target = st.settings.display.window_mode == "exclusive" ? "exclusive" : "borderless";
    else
        target = "windowed";
    set_window_mode(target);
    std::lock_guard<std::mutex> lk(st.mu);
    st.settings.display.window_mode = window_mode_name(platform::fullscreen_mode());
    st.dirty = true;
}

void toggle_fps_counter();       // defined after the settings page host

// Tells the overlay how the frame rate counter should look (the overlay draws it; nothing happens while it is off).
void apply_fps_counter(const FpsSettings& f) {
    G& st = g();
    if (!st.overlay) return;
    gfx::Overlay::CounterConfig c;
    c.show = f.show;
    c.corner = static_cast<int>(f.corner);
    c.detail = f.detail;
    c.in_screenshots = f.in_screenshots;
    st.overlay->set_counter(c);
}

// The pointer is over a slider's track in the game's menu (its menu hooks are active this frame): the wheel steps the value.
bool game_menu_over_value() {
    const GamePlay* gp = game_play();
    return gp && gp->menu_active && gp->menu_over_value && gp->menu_active() && gp->menu_over_value();
}

// The Joy-Con 2 mouse is on its surface while the game is in play (menus and the map keep their bindings and the menu pointer).
// [input] jc2_clicks: "both" (default) also hands clicks to the bindings (a real Joy-Con 2 sends its buttons as npad R/ZR),
// "device" sends them to the Joy-Con mouse only.
bool jc2_owns_clicks() {
    G& st = g();
    static const bool device_only = config_get_str("input", "jc2_clicks", "both") == "device";
    return device_only && st.engine && ctx_in_play(st.engine->context()) && hid::jc2_mouse_engaged();
}

void handle_event(const input::Event& e, uint64_t now) {
    G& st = g();
    using input::EventType;
    st.events++;
    // rebinding capture comes first: the page asked for the next input
    if (st.capture_state.load() == 1) {
        InputId id;
        bool got = false, cancel = false;
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) {
            if (e.u.key.scancode == SDL_SCANCODE_ESCAPE) cancel = true;
            else { id = {InKind::Key, e.u.key.scancode}; got = true; }
        } else if (e.type == EventType::MouseButtonDown) {
            id = {InKind::Mouse, e.u.button.button}; got = true;
        } else if (e.type == EventType::MouseWheel && e.u.wheel.dy != 0) {
            id = {InKind::Wheel, e.u.wheel.dy > 0 ? 1 : -1}; got = true;
        }
        if (got) {
            std::lock_guard<std::mutex> lk(st.cap_mu);
            st.cap_id = id;
            st.cap_have = true;
            st.capture_state.store(0);
            return;
        }
        if (cancel) { st.capture_state.store(0); return; }
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) return;    // nothing reaches the game while capturing
    }
    // The frame rate counter hotkey: flips the counter and never reaches the game (press, repeat and release).
    if (st.overlay && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && st.settings.fps.key.kind == InKind::Key &&
        e.u.key.scancode == st.settings.fps.key.code) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle_fps_counter();
        return;
    }
    if (st.overlay && st.overlay->handle_event(e)) return;
    const bool menu_blocked = st.overlay && st.overlay->blocks_game();
    if ((e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) || e.type == EventType::MouseButtonDown || e.type == EventType::MouseWheel)
        st.last_dev = LastDevice::KeyboardMouse;
    switch (e.type) {
        case EventType::Quit:
            if (st.quit_on_close) quit_now("window closed");
            break;
        case EventType::KeyDown:
            if ((e.u.key.scancode == SDL_SCANCODE_RETURN || e.u.key.scancode == SDL_SCANCODE_KP_ENTER) && (e.mods & SDL_KMOD_ALT)) {
                if (!(e.flags & input::kFlagRepeat)) toggle_fullscreen();
                return;
            }
            if (!(e.flags & input::kFlagRepeat)) mm::pointer().on_other_input();
            if (!(e.flags & input::kFlagRepeat) && !menu_blocked) hid::jc2_mouse_other_input();   // the last device used wins
            if (!(e.flags & input::kFlagRepeat) && !menu_blocked) st.engine->key(e.u.key.scancode, true, now);
            break;
        case EventType::KeyUp:
            st.engine->key(e.u.key.scancode, false, now);
            break;
        case EventType::MouseButtonDown:
            if (e.u.button.button == 1) st.mouse1 = true;
            st.jc2_buttons |= jc2_button_bit(e.u.button.button);
            if (!menu_blocked) st.jc2_presses |= jc2_button_bit(e.u.button.button);
            if (!menu_blocked) mm::pointer().on_button(e.u.button.button, true);
            // While the Joy-Con 2 mouse owns the mouse in play, the left and right buttons belong to the device alone (the middle button stays
            // the mouse-mode key). A release is always passed on: it only matters when the press was seen before the device took over.
            if (!menu_blocked && !(jc2_owns_clicks() && (e.u.button.button == 1 || e.u.button.button == 3))) st.engine->mouse_button(e.u.button.button, true, now);
            break;
        case EventType::MouseButtonUp:
            if (e.u.button.button == 1) st.mouse1 = false;
            st.jc2_buttons &= ~jc2_button_bit(e.u.button.button);
            mm::pointer().on_button(e.u.button.button, false);
            st.engine->mouse_button(e.u.button.button, false, now);
            break;
        case EventType::MouseWheel: {
            if (menu_blocked) break;
            st.jc2_wheel_x += e.u.wheel.dx;
            st.jc2_wheel_y += e.u.wheel.dy;
            if (jc2_owns_clicks()) break;           // the wheel goes to the Joy-Con 2 mouse only
            const float dy = e.u.wheel.dy;
            // the map: the wheel zooms (the L and R buttons), a longer pulse than a button tap so one notch is a visible step
            if (dy != 0 && st.settings.mouse.menu_cursor && st.engine->context() == Ctx::Map && !(st.overlay && st.overlay->visible())) {
                st.engine->pulse(dy > 0 ? Act::BtnR : Act::BtnL, now, 160);
                break;
            }
            // over a slider's track in a menu the wheel steps the value (right is more) instead of moving the selection
            if (dy != 0 && st.settings.mouse.menu_cursor && mm::pointer().enabled() && game_menu_over_value()) {
                st.engine->pulse(dy > 0 ? Act::DpadRight : Act::DpadLeft, now);
                break;
            }
            const int n = std::max(1, static_cast<int>(std::lround(std::fabs(dy))));
            if (dy != 0) for (int i = 0; i < n; ++i) st.engine->wheel(dy > 0 ? 1 : -1, now);
            break;
        }
        case EventType::MouseMotion:
            // the Joy-Con 2 mouse moves by the motion, captured (raw counts) or not (window pixels)
            if (!menu_blocked) {
                st.jc2_dx += e.u.motion.dx;
                st.jc2_dy += e.u.motion.dy;
            }
            if (!(e.flags & input::kFlagRelative)) {
                st.mouse_x = e.u.motion.x;
                st.mouse_y = e.u.motion.y;
                if (!menu_blocked) mm::pointer().on_motion(e.u.motion.x, e.u.motion.y);
                // moving the pointer over a menu counts as using the mouse (the prompts switch to keyboard icons); the raw motion of the aim in play does not
                if (!menu_blocked && st.settings.mouse.menu_cursor && mm::pointer().has_moved()) st.last_dev = LastDevice::KeyboardMouse;
            }
            break;
        case EventType::WindowFocusLost:
            audio::set_muted(st.window && st.settings.audio.mute_unfocused);
            st.engine->release_all(now);
            st.mouse1 = false;
            st.jc2_buttons = 0;
            mm::pointer().on_focus_lost();
            break;
        case EventType::WindowFocusGained:
            audio::set_muted(false);
            st.limiter.reset_schedule();
            break;
        case EventType::WindowRestored:
        case EventType::WindowMinimized:
            st.limiter.reset_schedule();
            break;
        case EventType::GamepadAdded:
            NXLOG_I(kTag, "controller %u connected", static_cast<unsigned>(e.u.gamepad.id));
            // a pad that replaces the one in use: the prompts follow its kind at once (without it they change at the first press)
            if (st.last_dev == LastDevice::Pad) st.last_pad_kind = static_cast<int>(input::gamepad_kind_of(e.u.gamepad.id));
            break;
        case EventType::GamepadRemoved:
            NXLOG_I(kTag, "controller %u disconnected", static_cast<unsigned>(e.u.gamepad.id));
            break;
        default: break;
    }
}

// Reads every open pad, merges them (buttons OR, the stick with the larger deflection wins) and publishes the result to the source.
void poll_pads(uint64_t now) {
    G& st = g();
    PadFrame pf;
    input::GamepadInfo infos[8];
    const size_t n = input::gamepad_list(infos, 8);
    bool have_six = false;
    uint16_t lo = 0, hi = 0;
    {
        const float l = std::max(st.vib[0][0].load(), st.vib[1][0].load()), h = std::max(st.vib[0][1].load(), st.vib[1][1].load());
        map_rumble(l, h, st.settings.pad, &lo, &hi);
        // 16 levels: the game changes the amplitude a little almost every frame in battle, and a report per frame over Bluetooth backs up
        // the pad's input until it lags by seconds (#26). Nonzero stays nonzero.
        auto q = [](uint16_t v) { return v ? static_cast<uint16_t>(std::max<unsigned>(v & 0xF000u, 0x1000u)) : uint16_t{0}; };
        lo = q(lo);
        hi = q(hi);
    }
    const Ctx cx = st.engine ? st.engine->context() : Ctx::Menu;
    const bool look_ctx = cx == Ctx::Play || cx == Ctx::Alt || cx == Ctx::Inspect;      // the right stick is a look only in play
    // A change goes out at most every 66 ms (about 15 a second); starting and stopping go out at once.
    const bool rumble_edge = (lo || hi) != (st.rumble_lo || st.rumble_hi);
    const bool rumble_changed = (lo != st.rumble_lo || hi != st.rumble_hi) && (rumble_edge || now - st.last_rumble_ns >= 66 * kMs);
    const bool rumble_refresh = (lo || hi) && now - st.last_rumble_ns > 100 * kMs;
    for (size_t i = 0; i < n; ++i) {
        platform::GamepadRaw raw;
        if (!platform::gamepad_poll(infos[i].id, &raw)) continue;
        hid::NpadInput tmp;
        map_pad(raw, st.settings.pad, &tmp);
        {   // a pressed button or a stick well off centre makes this the pad the prompts follow
            auto off_centre = [](hid::StickValue v) { return static_cast<int64_t>(v.x) * v.x + static_cast<int64_t>(v.y) * v.y > 20000ll * 20000ll; };
            if (tmp.buttons != 0 || off_centre(tmp.left) || off_centre(tmp.right)) {
                st.last_dev = LastDevice::Pad;
                st.last_pad_kind = static_cast<int>(infos[i].kind);
                mm::pointer().on_other_input();
                if (!(st.overlay && st.overlay->blocks_game())) hid::jc2_mouse_other_input();   // the last device used wins
            }
        }
        if (look_ctx) tmp.right = adjust_look_stick(tmp.right, st.settings.pad);
        pf.buttons |= tmp.buttons;
        auto big = [](hid::StickValue a, hid::StickValue b) {
            return static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.y) * a.y >= static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.y) * b.y ? a : b;
        };
        pf.left = big(pf.left, tmp.left);
        pf.right = big(pf.right, tmp.right);
        if (!have_six) {
            float a[3], gy[3];
            if (platform::gamepad_sensors(infos[i].id, a, gy)) {
                map_sensors(a, gy, st.settings.pad, &pf.six);
                have_six = true;
            }
        }
        // The haptic backends drive the pad themselves; only the plain two-motor path goes through SDL here.
        if ((rumble_changed || rumble_refresh) && haptic::legacy_rumble()) platform::gamepad_rumble(infos[i].id, lo, hi, 250);
    }
    if (rumble_changed || rumble_refresh) {
        st.rumble_lo = lo;
        st.rumble_hi = hi;
        st.last_rumble_ns = now;
    }
    // The on-screen keyboard reads the merged pad before it is held back; what it returns stays away from the game (everything while it is
    // open, then the button that closed it until it is released).
    const uint64_t raw_buttons = pf.buttons;
    if (st.overlay) {
        st.overlay->set_keyboard_abxy(st.settings.pad.label_abxy);
        pf.buttons &= ~st.overlay->set_keyboard_pad(pf.buttons, pf.left.x, pf.left.y);
        // the menu's pad chord and its gamepad navigation; the press that opens or closes it stays away from the game until released
        pf.buttons &= ~st.overlay->set_menu_pad(pf.buttons, pf.left.x, pf.left.y);
    }
    // The settings menu or the on-screen keyboard is open: the pad does not reach the game (the keyboard and mouse are held back the same way in
    // handle_event).
    if (st.overlay && st.overlay->blocks_game()) {
        static uint64_t s_held_logged = 0;   // test runs check this line: a new press while the menu is open is logged once
        if (const uint64_t fresh = raw_buttons & ~s_held_logged; fresh && tt::enabled())
            NXLOG_I(kTag, "menu open: pad buttons 0x%llx held back from the game (sent 0x%llx)", static_cast<unsigned long long>(raw_buttons),
                    static_cast<unsigned long long>(pf.buttons));
        s_held_logged = raw_buttons;
        pf.buttons = 0;
        pf.left = hid::StickValue{};
        pf.right = hid::StickValue{};
    }
    st.source_owner->set_pad(pf);
}

// set while update_hook runs: keyboard_service stays out of it
thread_local bool t_in_update = false;

int update_hook(::Ctx*, uint64_t, void*) {
    G& st = g();
    ft::mark("main.update");
    struct InUpdate {
        InUpdate() { t_in_update = true; }
        ~InUpdate() { t_in_update = false; }
    } in_update;
    // frame pacing (S18): the limiter waits here, before the input is sampled, so the picture shown is the one simulated one render time
    // earlier. The wait for frames still in flight comes first, then the limiter's schedule.
    // All of it runs once per presented frame: the game's main loop runs more iterations than it presents frames (Pokopia ~79 against ~50 a
    // second), and per iteration the limiter took several slots per frame while the in-flight list (one id per sim_begin, one popped per
    // present) filled up until wait_in_flight held the main thread for presents that only come after more main-loop work.
    const bool frame_start = extras::pace_boundary_due();
    if (frame_start) {
        extras::LowLatency* ll = extras::lowlat();
        if (extras::pace_in_main()) {
            if (ll) ll->wait_in_flight();
            // With Reflex on and a frame cap, the Reflex sleep that follows is the limiter (minimum interval between simulation starts)
            if (!(ll && ll->paces_frames())) { NXFT("limiter.main"); st.limiter.begin_frame_nohook(); }
            extras::pace_boundary_done();
        }
        // low latency (S14): the sleep that lets the frame start as late as the GPU allows, then the input sample marker; the events are pumped right after
        if (ll) {
            ll->sim_begin();
            ll->input_sample();
        }
    }
    const uint64_t now = now_ns();
    st.frames++;
    st.update_seq++;

    const bool on_pump_thread = std::this_thread::get_id() == st.pump_thread;
    if (on_pump_thread) g_last_pump_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    if (!on_pump_thread && !st.warned_thread) {
        st.warned_thread = true;
        st.thread_mismatch.store(true);
        NXLOG_W(kTag, "the frame-boundary hook runs on another thread than the one that started play mode: SDL events are not pumped");
    }
    if (st.window && on_pump_thread) platform::pump_events();

    // settings queued by the page
    {
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.have_pending) {
            Settings s = st.pending;
            st.have_pending = false;
            const std::string old_mode = st.settings.display.window_mode;
            const int old_monitor = st.settings.display.monitor;
            const int old_w = st.settings.display.width, old_h = st.settings.display.height;
            st.settings = s;
            st.settings_epoch++;
            lk.unlock();
            st.engine->set_settings(s, now);
            apply_frame_cap(s);
            display_tuning(s.display);
            if (const GamePlay* gp = game_play(); gp && gp->settings_changed) gp->settings_changed(s);     // the game's display options, lock-on
            extras::set_settings(s.extras);
            extras::hdr_refresh_levels();
            apply_fps_counter(s.fps);
            st.want_present.store(static_cast<int>(present_mode_for(s.display)));
            apply_audio(s.audio);
            haptic::configure(static_cast<haptic::Mode>(s.pad.haptic), s.pad.haptic_strength, s.pad.rumble_scale, s.pad.rumble);
            if (st.window && s.display.monitor != old_monitor && s.display.monitor > 0) {
                if (platform::move_to_display(s.display.monitor)) st.limiter.reset_schedule();
                apply_frame_cap(s);                        // another display can mean another refresh rate
            }
            if (s.display.window_mode != old_mode) {
                set_window_mode(s.display.window_mode);
                apply_frame_cap(s);                        // another window mode can mean another refresh rate (vsync cap)
            }
            // The windowed size is live while the window is windowed; in fullscreen it is the size the window goes back to (the next start).
            if (st.window && (s.display.width != old_w || s.display.height != old_h) && platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
                platform::set_window_size(s.display.width, s.display.height);
            std::string err;
            if (!save_settings(st.path, s, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
            else NXLOG_I(kTag, "settings applied and saved to %s", st.path.c_str());
        }
    }

    apply_unfocused_mute();

    // events
    input::Event buf[128];
    for (;;) {
        const size_t got = input::queue_drain(buf, 128);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    const GamePlay* gp = game_play();
    if (gp && gp->frame_begin) gp->frame_begin();
    if (st.engine) st.engine->frame_tick();

    // what the game is doing: the context, the cycle hint, the pointer
    const uint64_t pi = st.last_pi_ns.load();
    // "In game" = the game called ProcessInput during one of the last few frame boundaries. Counted in frames, not host time: a slow frame (the game
    // can take 100 ms or more per frame under load) must not read as a menu.
    // Also within 100 ms of host time: a catch-up burst of frame boundaries with no input call (1-2 frames about once a second at ~50 fps)
    // dropped the context to the menu and released the mouse for a moment.
    const bool in_game = pi != 0 && (st.frames.load() - st.last_pi_frame.load() <= 4 || pi + 100000000ull >= now);
    // The game's part (play_game.h GamePlay::frame): the gyro's pitch-rate scale for this step, the cycle hint, the context while in game. This
    // step's length is known here, before the game reads the sample (the timestep hook on the same frame update runs first: hooks of one address run
    // in registration order).
    // One of the game's menus over play (bag, shop, choice box: GamePlay::menu_in_game): the frame is a menu frame for the context, the
    // pointer and the mouse capture (with the menu pointer off, play stays play).
    const bool game_menu = gp && gp->menu_in_game && gp->menu_in_game(in_game && st.settings.mouse.menu_cursor) && in_game;
    const bool playing = in_game && !game_menu;
    GameFrame gf;
    if (gp && gp->frame) {
        const double step = timestep::active() ? static_cast<double>(timestep::current_dt()) : static_cast<double>(timestep::kGameStep);
        gp->frame(playing, step, &gf);
    }
    if (st.source_owner) st.source_owner->set_look_scale(gf.look_scale);
    if (gf.hints_valid) st.engine->set_cycle_hint(gf.cycle_index, gf.cycle_mask, now);
    const uint64_t map_acts = st.engine->activations(Act::Map);
    if (in_game && map_acts != st.map_acts_seen) st.map_hint = true;
    st.map_acts_seen = map_acts;
    if (in_game && !st.was_in_game) st.map_hint = false;
    st.was_in_game = in_game;
    Ctx ctx;
    if (!in_game) ctx = st.map_hint ? Ctx::Map : Ctx::Menu;
    else if (game_menu) ctx = Ctx::Menu;
    else ctx = gf.play_ctx;
    st.engine->set_context(ctx, now);

    {   // button prompts: the style that shows, and the icon textures behind it
        PromptInputs pin;
        pin.requested = st.settings.prompts.style;
        pin.label_abxy = st.settings.pad.label_abxy;
        pin.ctx = ctx;
        pin.settings_epoch = st.settings_epoch;
        pin.last = st.last_dev;
        pin.last_pad_kind = st.last_pad_kind;
        input::GamepadInfo pads[1];
        pin.connected_pad_kind = input::gamepad_list(pads, 1) > 0 ? static_cast<int>(pads[0].kind) : -1;
        prompts_update(st.settings, pin);
    }

    const bool overlay_visible = st.overlay && st.overlay->blocks_game();
    if (overlay_visible && !st.overlay_was_visible) st.engine->release_all(now);
    st.overlay_was_visible = overlay_visible;

    if (st.window) {
        platform::set_text_input(st.overlay && st.overlay->keyboard_open());   // typed text for the on-screen keyboard only
        const bool free_ok = platform::focused() && !platform::minimized() && !overlay_visible && st.capture_state.load() == 0;
        const bool want_cam = st.capture_mouse && free_ok && playing && !hid::jc2_mouse_wants_pointer();
        // The Joy-Con 2 mouse on its surface: the game draws its own cursor and turns its camera by the motion, so the mouse is captured
        // (raw counts, no OS cursor, no window edge) until another device lifts it ([mouse] jc2_capture).
        const bool want_jc2 = st.settings.mouse.jc2_capture && free_ok && hid::jc2_mouse_engaged();
        const bool want = want_cam || want_jc2;
        if (want != platform::relative_mouse()) {
            platform::set_relative_mouse(want);
            if (want) input::mouse_delta_take();      // nothing that piled up before the capture
            // The Joy-Con 2 mouse lifted: the Windows pointer comes back where the game's own pointer is (the game layer says where).
            float ru = 0, rv = 0;
            if (!want && st.jc2_captured && hid::jc2_mouse_take_return_point(&ru, &rv)) {
                mm::Geometry g2;
                platform::pixel_size(&g2.win_w, &g2.win_h);
                const DisplayLive dl2 = gp && gp->display_live ? gp->display_live() : DisplayLive{};
                g2.render_w = dl2.render_w;
                g2.render_h = dl2.render_h;
                g2.display_aspect = dl2.aspect > 0 ? dl2.aspect : 0;
                g2.keep_aspect = st.settings.display.keep_aspect;
                const mm::Rect r = mm::picture_rect(g2);
                const float wx = static_cast<float>(r.x + std::clamp(ru, 0.f, 1.f) * r.w), wy = static_cast<float>(r.y + std::clamp(rv, 0.f, 1.f) * r.h);
                platform::warp_mouse(wx, wy);
                st.mouse_x = wx;
                st.mouse_y = wy;
                if (tt::enabled()) NXLOG_I(kTag, "Windows pointer put back at the game's pointer: %.3f,%.3f on the picture = window %.0f,%.0f", ru, rv, wx, wy);
            }
            if (tt::enabled() || want_jc2 != st.jc2_captured)
                NXLOG_I(kTag, "mouse %s%s", want ? "captured" : "released", want_jc2 ? " (Joy-Con 2 mouse)" : want_cam ? " (camera)" : "");
        }
        st.jc2_captured = want && want_jc2;
    }
    {   // the map: dragging with the left button rotates it (the left stick), with the middle button moves it (the right stick); the speed is the pointer's speed
        static float px = 0, py = 0;
        static uint64_t last_ns = 0;
        static bool have = false;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        mm::Pointer& pp = mm::pointer();
        const bool on_map = st.window && ctx == Ctx::Map && st.settings.mouse.menu_cursor && pp.enabled() && (platform::focused() || tt::enabled()) && !overlay_visible;
        if (on_map && have && (pp.held() || pp.mid_held())) {
            int ww = 1, wh = 1;
            platform::pixel_size(&ww, &wh);
            // the stick follows the pointer's speed: a full stick is a drag of 0.8 window heights per second, whatever the frame rate
            const float dt = std::max(1e-3f, static_cast<float>(now - last_ns) / 1e9f);
            const float k = 1.f / (0.8f * static_cast<float>(std::max(1, wh)) * dt);
            const float dx = std::max(-1.f, std::min(1.f, (pp.x() - px) * k)), dy = std::max(-1.f, std::min(1.f, (pp.y() - py) * k));
            if (pp.held()) { lx = dx; ly = -dy; }
            else { rx = dx; ry = -dy; }
        }
        px = pp.x();
        py = pp.y();
        last_ns = now;
        have = true;
        st.engine->set_aux_sticks(lx, ly, rx, ry);
    }
    {   // the mouse as a pointer in the menus (S28): the cursor, and the snapshot the widget hooks read this frame
        mm::Pointer& ptr = mm::pointer();
        ptr.set_enabled(st.settings.mouse.menu_cursor);
        // (a scripted run has no focus: the test tooling drives the pointer with synthetic events)
        const bool menu_up = st.window && !playing && ctx == Ctx::Menu && (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
        if (tt::enabled()) {   // test runs: why the menu pointer is or is not up, on each change
            static int last = -1;
            const int sig = (st.window ? 1 : 0) | (playing ? 2 : 0) | (static_cast<int>(ctx) << 2) | (overlay_visible ? 256 : 0);
            if (sig != last && (last = sig, true))
                NXLOG_I("play", "menu pointer gate: window %d playing %d ctx %d (menu %d) minimized %d overlay %d -> %d", st.window ? 1 : 0, playing ? 1 : 0,
                        static_cast<int>(ctx), static_cast<int>(Ctx::Menu), platform::minimized() ? 1 : 0, overlay_visible ? 1 : 0, menu_up ? 1 : 0);
        }
        mm::Geometry geo;
        platform::pixel_size(&geo.win_w, &geo.win_h);
        const DisplayLive dl = gp && gp->display_live ? gp->display_live() : DisplayLive{};
        geo.render_w = dl.render_w;
        geo.render_h = dl.render_h;
        geo.display_aspect = dl.aspect > 0 ? dl.aspect : 0;
        geo.keep_aspect = st.settings.display.keep_aspect;
        if (const Act a = gp && gp->menu_frame ? gp->menu_frame(menu_up, geo) : Act::None; a != Act::None) st.engine->pulse(a, now);
        if (st.overlay && st.pointer_draw) st.overlay->set_pointer(menu_up && ptr.enabled() && ptr.cursor_visible(menu_up), ptr.x(), ptr.y(), true);
        // With the Joy-Con 2 mouse on its surface the game draws its own cursor: the OS cursor is hidden (unless the overlay is up).
        const bool game_cursor = !overlay_visible && hid::jc2_mouse_engaged();
        if (st.window && !platform::relative_mouse())
            platform::set_cursor_visible(!game_cursor && (overlay_visible || !(ptr.enabled() && menu_up) || ptr.cursor_visible(menu_up)));
        if (st.window) {   // the Joy-Con 2 mouse (hid): the motion, the buttons and the wheel
            hid::HostMouse hm;
            const bool focused = (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
            // motion while the mouse is free counts only over the picture (crossing the window on the way elsewhere wakes nothing)
            bool inside = true;
            if (!platform::relative_mouse()) {
                mm::window_to_picture(geo, st.mouse_x, st.mouse_y, &hm.u, &hm.v, &inside);
                hm.pointer = inside;
            }
            const float k = static_cast<float>(st.settings.mouse.jc2_speed);
            if (focused && inside) {
                hm.dx = st.jc2_dx * k;
                hm.dy = st.jc2_dy * k;
                hm.wake = std::sqrt(st.jc2_dx * st.jc2_dx + st.jc2_dy * st.jc2_dy);
            }
            hm.buttons = overlay_visible ? 0u : st.jc2_buttons;
            hm.presses = focused && inside ? st.jc2_presses : 0u;
            hm.wheel_x = st.jc2_wheel_x;
            hm.wheel_y = st.jc2_wheel_y;
            hm.focused = focused;
            hid::jc2_mouse_set_wake(static_cast<float>(st.settings.mouse.jc2_wake));
            hid::jc2_mouse_feed(hm);
            st.jc2_wheel_x = st.jc2_wheel_y = 0;
            st.jc2_dx = st.jc2_dy = 0;
            st.jc2_presses = 0;
            // the mouse in use as the Joy-Con 2 mouse: the prompts follow it like any other mouse use
            const bool engaged = hid::jc2_mouse_engaged();
            if (engaged && (hm.wake > 0 || hm.presses)) st.last_dev = LastDevice::KeyboardMouse;
            // Its clicks: a Joy-Con 2 (R) used as a mouse has R under the index finger and ZR under the middle one, and the game reads them
            // as npad buttons ("Left click (R): Confirm, Right click (ZR): Cancel", 0_ISM_Battle.msbt). Only while the mouse is on its
            // surface: the click that puts it down is not also a button press. Held at least 50 ms so a quick click is seen.
            uint64_t clicks = 0;
            if (engaged && focused) {
                constexpr uint32_t kBits[2] = {1u << 0, 1u << 1};
                const uint64_t kNpad[2] = {hid::btn(hid::kBtnR), hid::btn(hid::kBtnZR)};
                for (int i = 0; i < 2; ++i) {
                    if (hm.presses & kBits[i]) st.jc2_click_until[i] = now + 50 * kMs;
                    if ((st.jc2_buttons & kBits[i]) || now < st.jc2_click_until[i]) clicks |= kNpad[i];
                }
            } else {
                st.jc2_click_until[0] = st.jc2_click_until[1] = 0;
            }
            st.source_owner->set_jc2_buttons(clicks);
        }
    }
    if (!playing) input::mouse_delta_take();         // aim only counts what arrives while the game is playing (not under one of its menus)

    if (st.log_status && now - st.last_status_ns > 1000 * kMs) {
        st.last_status_ns = now;
        GameStatus gs;
        if (gp && gp->status) gp->status(&gs);
        char game_part[512] = "";
        if (gp && gp->status_text) gp->status_text(game_part, sizeof game_part);
        NXLOG_I(kTag, "status: ctx=%s in_game=%d aim=%s frames=%llu process_input=%llu aim_frames=%llu events=%llu pads=%zu captured=%d%s", ctx_name(ctx),
                in_game ? 1 : 0, gs.aim, static_cast<unsigned long long>(st.frames.load()),
                static_cast<unsigned long long>(st.pi_calls.load()), static_cast<unsigned long long>(st.aim_frames.load()),
                static_cast<unsigned long long>(st.events.load()), input::gamepad_count(), platform::relative_mouse() ? 1 : 0, game_part);
    }
    if (st.selftest.active()) st.selftest.tick(st.renderer, &st.limiter, selftest_host());
    return 0;
}

// The software keyboard applet blocks the game: the thread that asked waits for the answer, the main thread soon waits too, and no frame is
// presented (Pokopia's name entry: the freeze watchdog fired 10 s after the question, frame count still). update_hook, which pumps the window
// and feeds the overlay, does not run then. keyboard_service does that part on the window's thread while it is blocked in the kernel
// (kernel::set_blocked_hook) or about to make an SVC (busy_pump), every 8 ms at most: the window's events and the pads go to the overlay's
// on-screen keyboard, the mouse is free, typing is on, and the game's last frame is presented again so the keyboard is drawn. Nothing
// happens while no question is open, or from inside update_hook.
void keyboard_service() {
    static thread_local uint64_t last_ms = 0;
    const uint64_t t = SDL_GetTicks();
    if (t - last_ms < 8 || t_in_update) return;
    last_ms = t;
    G& st = g();
    if (!st.started || !st.overlay || !st.overlay->keyboard_open()) return;
    const uint64_t now = now_ns();
    platform::pump_events();          // also headless (scripted runs): SDL's virtual pads update here
    if (st.window) {
        g_last_pump_ms.store(t, std::memory_order_relaxed);
        platform::set_text_input(true);
        if (platform::relative_mouse()) platform::set_relative_mouse(false);
        platform::set_cursor_visible(true);
    }
    input::Event buf[64];
    for (;;) {
        const size_t got = input::queue_drain(buf, 64);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    nvn::present_last_frame_again();
}

// ---- what the game module gets from play mode (play_game.h GameHost) ----------------------------------------------------------------------
class GameHostImpl : public GameHost {
public:
    const Settings& settings() const override { return g().settings; }
    bool action_active(Act a, uint64_t now_ns) const override { return g().engine && g().engine->act_active(a, now_ns); }
    uint64_t action_activations(Act a) const override { return g().engine ? g().engine->activations(a) : 0; }
    void right_stick(float* x, float* y) const override {
        *x = *y = 0;
        if (g().source_owner) g().source_owner->right_stick(x, y);
    }
    bool pad_look(bool gyro_enabled) const override { return g().source_owner && g().source_owner->pad_look(gyro_enabled); }
    bool has_input() const override { return g().source_owner != nullptr; }
    void input_tick(double* dx, double* dy) override {
        G& st = g();
        const uint64_t now = now_ns();
        st.last_pi_ns.store(now);
        st.last_pi_frame.store(st.frames.load());
        st.pi_calls++;
        *dx = *dy = 0;
        if (st.update_seq != st.aim_seq) {                // the first input tick since the frame boundary takes the mouse counts
            st.aim_seq = st.update_seq;
            // Late sample: the frame boundary pumped the events at the start of the frame update; the tick that reaches the input function is a little
            // later (the whole update body runs first). Pump again so the counts include whatever the mouse did in between. Main thread only, like the
            // pump in the frame-boundary hook; the key and button events it queues are handled at the next boundary.
            if (st.late_sample && st.window && std::this_thread::get_id() == st.pump_thread) platform::pump_events();
            if (extras::LowLatency* ll = extras::lowlat()) ll->input_sample();
            const input::MouseDelta d = input::mouse_delta_take();
            *dx = d.dx;
            *dy = d.dy;
        }
    }
    void aim_applied(double dx, double dy) override {
        G& st = g();
        st.aim_frames++;
        st.counts += static_cast<uint64_t>(std::fabs(dx) + std::fabs(dy));
    }
    double pitch_limit_deg() const override { return g().pitch_limit_deg; }
    bool log_status() const override { return g().log_status; }
};
GameHostImpl g_game_host;

// ---- the settings page host -----------------------------------------------------------------------------------------------------------
class Host : public UiHost {
public:
    Settings settings() const override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); return st.have_pending ? st.pending : st.settings; }
    Settings factory() const override { return default_settings(); }
    void apply(const Settings& s) override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); st.pending = s; st.have_pending = true; }
    LiveStatus status() const override {
        G& st = g();
        LiveStatus s;
        s.ctx = st.engine ? st.engine->context() : Ctx::Menu;
        GameStatus gs;
        if (const GamePlay* gp = game_play(); gp && gp->status) gp->status(&gs);
        s.aim = gs.aim;
        s.prompt_now = prompts_effective_name();
        if (st.pi_calls.load() == 0) s.aim = "not in game";
        s.mouse_captured = st.window && platform::relative_mouse();
        input::GamepadInfo infos[4];
        s.pads = static_cast<int>(input::gamepad_count());
        if (input::gamepad_list(infos, 4) > 0) {
            s.pad_name = infos[0].name;
            s.pad_gyro = infos[0].gyro;
            s.pad_rumble = infos[0].rumble;
        }
        if (st.renderer) {
            s.present_mode = gfx::present_mode_name(st.renderer->present_mode());
            const uint64_t now = now_ns();
            const uint64_t frames = st.renderer->stats().frames;
            std::lock_guard<std::mutex> lk(st.st_mu);
            if (now - st.fps_last_ns > 1000 * kMs) {
                if (st.fps_last_ns) st.fps = static_cast<double>(frames - st.fps_last_frames) * 1e9 / static_cast<double>(now - st.fps_last_ns);
                st.fps_last_frames = frames;
                st.fps_last_ns = now;
            }
            s.fps = st.fps;
        }
        if (st.window) {
            s.window_mode = window_mode_name(platform::fullscreen_mode());
            s.refresh_hz = platform::display_refresh_hz();
            platform::pixel_size(&s.width, &s.height);
        }
        s.out = st.engine->output(now_ns());
        s.pitch_deg = gs.pitch_rad * 180.0 / kPi;
        s.input_calls = st.pi_calls.load();
        return s;
    }
    std::string settings_path() const override { return g().path; }
    void begin_capture() override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        st.cap_have = false;
        st.capture_state.store(1);
    }
    bool capturing() const override { return g().capture_state.load() == 1; }
    bool capture_result(InputId* out) override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        if (!st.cap_have) return false;
        st.cap_have = false;
        if (out) *out = st.cap_id;
        return true;
    }
    void cancel_capture() override { g().capture_state.store(0); }
    bool reserved_key(const InputId& id) const override {
        G& st = g();
        return st.overlay && id.kind == InKind::Key && id.code == st.overlay->toggle_scancode();
    }
};
Host g_host;

}  // namespace
UiHost* ui_host() { return &g_host; }
namespace {

void toggle_fps_counter() {
    Settings s = g_host.settings();
    s.fps.show = !s.fps.show;
    g_host.apply(s);
}

// The self-test's window onto play mode: the same entry points the settings page uses.
class SelfTestHostImpl : public SelfTestHost {
public:
    bool set(const std::string& key, const std::string& value) override {
        G& st = g();
        Settings s = g_host.settings();
        const double d = std::atof(value.c_str());
        if (key == "vsync") s.display.vsync = d != 0;
        else if (key == "fps_cap" || key == "fps_limit") s.display.fps_cap = static_cast<int>(d);
        else if (key == "window_mode") s.display.window_mode = value;
        else if (key == "yaw_source") s.mouse.yaw_rad = rad_per_count(d);
        else if (key == "pitch_source") s.mouse.pitch_rad = rad_per_count(d);
        else if (key == "invert_y") s.mouse.invert_y = d != 0;
        else if (key == "invert_x") s.mouse.invert_x = d != 0;
        else if (key == "fps") s.fps.show = d != 0;
        else if (key == "fps_in_shots") s.fps.in_screenshots = d != 0;
        else if (key == "fps_corner") {
            if (!parse_corner(value, &s.fps.corner)) return false;
        } else if (key == "fps_detail") s.fps.detail = std::clamp(static_cast<int>(d), 0, 2);
        else if (key == "prompt_style") {
            if (!parse_prompt_style(value, &s.prompts.style)) return false;
        } else return false;
        g_host.apply(s);
        (void)st;
        return true;
    }
    std::string present_mode() override { G& st = g(); return st.renderer ? gfx::present_mode_name(st.renderer->present_mode()) : ""; }
    std::string settings_text() override {
        G& st = g();
        std::string text;
        if (FILE* f = std::fopen(st.path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    }
};

SelfTestHost* selftest_host() {
    static SelfTestHostImpl h;
    return &h;
}

// mpr.ini and the environment override the settings file for the display extras. A key that is set there ([hdr] mode in mpr.ini, or NXRT_HDR_MODE)
// replaces the file's value with the one extras::settings_from_config() reads; other keys keep the file's value (the F1 page edits the file).
extras::Settings merge_env(const extras::Settings& from_file) {
    const extras::Settings env = extras::settings_from_config();
    extras::Settings r = from_file;
    if (config_has("hdr", "mode")) r.hdr.mode = env.hdr.mode;
    if (config_has("hdr", "format")) r.hdr.format = env.hdr.format;
    if (config_has("hdr", "peak_nits")) r.hdr.peak_nits = env.hdr.peak_nits;
    if (config_has("hdr", "paper_white_nits")) r.hdr.paper_white_nits = env.hdr.paper_white_nits;
    if (config_has("hdr", "highlight_boost")) r.hdr.highlight_boost = env.hdr.highlight_boost;
    if (config_has("upscale", "mode")) r.upscale.mode = env.upscale.mode;
    if (config_has("upscale", "sharpness")) r.upscale.sharpness = env.upscale.sharpness;
    if (config_has("upscale", "dlss_preset")) r.upscale.dlss_preset = env.upscale.dlss_preset;
    if (config_has("upscale", "quality")) r.upscale.quality = env.upscale.quality;
    if (config_has("upscale", "scene_scale")) r.upscale.scene_scale = env.upscale.scene_scale;
    if (config_has("upscale", "fsr_dll")) r.upscale.fsr_dll = env.upscale.fsr_dll;
    if (config_has("upscale", "dlss_dll")) r.upscale.dlss_dll = env.upscale.dlss_dll;
    if (config_has("upscale", "xess_dll")) r.upscale.xess_dll = env.upscale.xess_dll;
    if (config_has("upscale", "frame_generation")) r.upscale.frame_generation = env.upscale.frame_generation;
    if (config_has("latency", "mode")) r.latency.mode = env.latency.mode;
    if (config_has("latency", "log_timings")) r.latency.log_timings = env.latency.log_timings;
    // [picture]: a preset or any of the four keys in mpr.ini / the environment replaces the file's picture controls as a whole
    if (config_has("picture", "preset") || config_has("picture", "contrast") || config_has("picture", "saturation") || config_has("picture", "black") ||
        config_has("picture", "gamma"))
        r.picture = env.picture;
    return r;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------------------------------------
bool load() {
    G& st = g();
    if (st.loaded) return st.load_ok;
    st.loaded = true;
    st.path = config_get_str("play", "controls", "controls.ini");
    std::string err;
    const bool existed = GetFileAttributesA(st.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    st.load_ok = load_settings(st.path, &st.settings, &err);
    if (!existed) {
        std::string e2;
        if (save_settings(st.path, st.settings, &e2)) NXLOG_I(kTag, "wrote the factory settings to %s", st.path.c_str());
    } else if (!st.load_ok) {
        NXLOG_W(kTag, "%s: %s (the factory settings are in use)", st.path.c_str(), err.c_str());
    } else {
        NXLOG_I(kTag, "settings loaded from %s (%zu bindings)", st.path.c_str(), st.settings.bindings.size());
    }
    // An older controls.ini got the newer factory bindings (play_config.cpp, kDefaultAdditions): keep the old file once as .bak, then
    // write the upgraded one. CopyFileA with bFailIfExists = TRUE leaves an existing .bak alone.
    if (existed && st.load_ok && st.settings.migrated) {
        const std::string bak = st.path + ".bak";
        if (CopyFileA(st.path.c_str(), bak.c_str(), TRUE)) NXLOG_I(kTag, "kept the previous %s as %s", st.path.c_str(), bak.c_str());
        std::string e3;
        if (save_settings(st.path, st.settings, &e3))
            NXLOG_I(kTag, "controls.ini updated to defaults revision %d (added: %s)", st.settings.defaults_rev,
                    st.settings.migration_added.empty() ? "nothing new" : st.settings.migration_added.c_str());
        else
            NXLOG_W(kTag, "cannot save the upgraded %s: %s", st.path.c_str(), e3.c_str());
    }
    // Display extras: a value set in mpr.ini or in the environment (NXRT_HDR_MODE ...) overrides the file; the file overrides the built-in defaults.
    st.settings.extras = merge_env(st.settings.extras);
    extras::set_settings(st.settings.extras);
    // S38 GPU paths: the file's choice becomes [nvn] pool_mirror / pass_barriers unless mpr.ini or the environment set them. The replay
    // reads both once, at its first pool and its first submission, after this.
    if (!config_has("nvn", "pool_mirror")) config_set("nvn", "pool_mirror", st.settings.display.fast_gpu_memory ? "on" : "off");
    if (!config_has("nvn", "pass_barriers")) config_set("nvn", "pass_barriers", st.settings.display.fewer_gpu_waits ? "tracked" : "full");
    // Prewarm: the file's choice becomes [nvn] prewarm unless the ini or the environment set it (read once, by the pipeline prewarm).
    if (!config_has("nvn", "prewarm")) config_set("nvn", "prewarm", st.settings.display.shader_prewarm ? "true" : "false");
    // ASTC texture quality: the file's choice becomes [gfx] astc_quality unless the ini or the environment set it (the texture back end reads it at its first ASTC texture).
    if (!config_has("gfx", "astc_quality")) config_set("gfx", "astc_quality", st.settings.display.astc_quality.c_str());
    // ASTC storage (BC1 for opaque textures on small GPUs): the same, as [gfx] astc_bc (read once, at the first ASTC image).
    if (!config_has("gfx", "astc_bc")) config_set("gfx", "astc_bc", st.settings.display.astc_bc.c_str());
    // Audio: a volume or device set in the ini or the environment wins over the file; the page then shows what is in force.
    if (config_has("audio", "volume")) st.settings.audio.volume = audio::master_volume();
    else audio::set_master_volume(static_cast<float>(st.settings.audio.volume));
    if (config_has("audio", "device")) st.settings.audio.device = config_get_str("audio", "device", "default");
    else config_set("audio", "device", st.settings.audio.device.c_str());
    // Speakers: the file's choice becomes [audio] channels unless the ini or the environment set it. The audio services read it when the
    // game opens its audio renderer (services/audio.cpp configured_channels, audren HostAudioDevice), which is after this.
    if (config_has("audio", "channels")) st.settings.audio.channels = config_get_int("audio", "channels", 2) == 6 ? 6 : 2;
    else config_set("audio", "channels", st.settings.audio.channels == 6 ? "6" : "2");
    st.capture_mouse = config_get_bool("play", "capture_mouse", true);
    st.late_sample = config_get_bool("play", "late_sample", true);
    st.quit_on_close = config_get_bool("play", "quit_on_close", true);
    st.log_status = config_get_bool("play", "log_status", false);
    st.pointer_draw = config_get_bool("play", "pointer_draw", false);
    st.pitch_limit_deg = config_get_double("play", "pitch_limit_deg", 0.0);
    st.base = static_cast<uint64_t>(config_get_int("play", "game_base", static_cast<int64_t>(game::info().main_base)));
    apply_frame_cap(st.settings);
    return st.load_ok;
}

DisplayConfig display() {
    G& st = g();
    load();
    DisplayConfig d;
    d.width = st.settings.display.width;
    d.height = st.settings.display.height;
    d.window_mode = st.settings.display.window_mode;
    d.vsync = st.settings.display.vsync;
    d.fps_cap = st.settings.display.fps_cap;
    d.keep_aspect = st.settings.display.keep_aspect;
    d.present_mode = present_mode_for(st.settings.display);
    return d;
}

FrameLimiter* limiter() {
    load();
    return &g().limiter;
}

std::string settings_path() {
    load();
    return g().path;
}

bool start(const StartOptions& opt) {
    G& st = g();
    load();
    if (st.started) return true;
    st.renderer = opt.renderer;
    st.device = opt.device;
    st.window = opt.window && platform::has_window();
    apply_frame_cap(st.settings);                      // the display's refresh rate is known now (vsync cap)
    kernel::sched_set_process_priority(st.window);     // a person is playing: keep the frame out of the background load
    st.pump_thread = std::this_thread::get_id();
    g_pump_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    st.engine = std::make_unique<Engine>(st.settings);
    st.engine->set_context(Ctx::Menu, now_ns());
    st.source_owner = std::make_unique<PlaySource>(st.engine.get());
    st.source = std::shared_ptr<hid::InputSource>(st.source_owner.get(), [](hid::InputSource*) {});   // owned by G
    hid::set_input_source(st.source);
    hid::set_vibration_sink(&vib_sink, nullptr);
    haptic::configure(static_cast<haptic::Mode>(st.settings.pad.haptic), st.settings.pad.haptic_strength, st.settings.pad.rumble_scale, st.settings.pad.rumble);
    tt::set_mouse_sink(&test_mouse_sink);
    tt::set_key_sink(&key_sink);
    if (st.renderer) tt::set_screenshot_provider(&screenshot_provider, nullptr);
    if (const std::string script = config_get_str("play", "selftest", ""); !script.empty()) {
        std::string err;
        if (!st.selftest.parse(script, &err)) {
            NXLOG_E(kTag, "[play] selftest: %s", err.c_str());
            nxrt::log_flush();
            TerminateProcess(GetCurrentProcess(), 2);
        }
        NXLOG_I(kTag, "selftest script: %s", script.c_str());
    }

    if (st.renderer) {
        st.limiter.set_pre_frame_hook(&pre_frame, nullptr);
        if (!st.renderer->headless()) st.renderer->set_present_mode(present_mode_for(st.settings.display));
        // [applet] swkbd_prompt = force makes the overlay for the on-screen keyboard even where the host asked for none (headless test runs)
        const bool swkbd_force = config_get_str("applet", "swkbd_prompt", "true") == "force";
        if ((opt.overlay || swkbd_force) && opt.device) {
            gfx::Overlay::Context oc;
            oc.device = opt.device;
            oc.renderer = opt.renderer;
            oc.limiter = &st.limiter;
            oc.has_window = st.window;
            std::string err;
            st.overlay = gfx::Overlay::create(oc, &err);
            if (!st.overlay) {
                NXLOG_W(kTag, "no overlay: %s", err.c_str());
            } else {
                if (const GamePlay* gp = game_play(); gp && gp->settings_menu) {   // the game's own menu (GamePlay::settings_menu)
                    gfx::Overlay* ov = st.overlay.get();
                    st.overlay->set_menu([gp, ov] { gp->settings_menu(g_host, *ov); });
                } else {
                    st.overlay->add_panel("Settings", [] { draw_settings_page(g_host); });
                }
                st.overlay->install_on_renderer();
                apply_fps_counter(st.settings.fps);
            }
        }
    }
    // The software keyboard asks the player on the overlay's on-screen keyboard while there is a game window ([applet] swkbd_prompt = false: never;
    // force: also without a window, for scripted headless runs).
    {
        const bool force = config_get_str("applet", "swkbd_prompt", "true") == "force";
        services::swkbdui::enable_prompt(st.overlay && (force || (st.window && config_get_bool("applet", "swkbd_prompt", true))));
    }
    // the window keeps answering while the game boots or loads (no frame boundary for seconds), and the on-screen keyboard works while the
    // game waits for it (keyboard_service; also headless with swkbd_prompt = force, which has an overlay and no window)
    if (st.window || st.overlay) svc_set_busy_hook(&busy_pump);
    kernel::set_blocked_hook(GetCurrentThreadId(), &keyboard_service);
    if (st.window && st.settings.display.monitor > 0) platform::move_to_display(st.settings.display.monitor);
    if (st.window && st.settings.display.window_mode != "windowed") set_window_mode(st.settings.display.window_mode);

    const GamePlay* gp = game_play();
    {   // display options: the internal resolution comes from the window as it is now (after the window mode change above)
        int ww = 0, wh = 0;                    // no window (headless, test runs): the configured window size, not a made-up one
        if (st.window) platform::pixel_size(&ww, &wh);
        if (ww <= 0 || wh <= 0) { ww = st.settings.display.width; wh = st.settings.display.height; }
        display_tuning(st.settings.display);
        if (gp && gp->install_render) gp->install_render(st.base, st.settings.display, ww, wh);   // the game's display options, tone curve
    }
    // the frame boundary, then the game's input, camera, lock-on and menu hooks (hooks of one address run in registration order)
    if (gp && gp->frame_update) st.hook_update = hook_add_entry(st.base + gp->frame_update, &update_hook);
    else NXLOG_W(kTag, "the game (%s) names no frame update: play mode is not pumped", game::info().id);
    if (gp && gp->install) gp->install(st.base, &g_game_host, st.settings);
    // code mods (mods/<name>/exefs/*.dll, nxrt/mod_api.h) after the game's own hooks, so theirs run after the game's at a shared site
    services::modapi::load_dll_mods(st.base, gp ? gp->frame_update : 0);
    apply_frame_cap(st.settings);      // the game's hooks may have set a frame cap floor (frame_limiter.h)
    st.started = true;
    NXLOG_I(kTag, "play mode started (%s, overlay %s, base 0x%llx, game %s, frame hook %u)", st.window ? "window" : "no window", st.overlay ? "on" : "off",
            static_cast<unsigned long long>(st.base), game::info().id, st.hook_update);
    return true;
}

void stop() {
    G& st = g();
    if (!st.started) return;
    vpad_shutdown();      // a virtual pad from a test script does not outlive play mode
    if (const GamePlay* gp = game_play(); gp && gp->remove) gp->remove();
    kernel::set_blocked_hook(0, nullptr);
    if (st.hook_update) hook_remove(st.hook_update);
    st.hook_update = 0;
    hid::set_vibration_sink(nullptr, nullptr);
    haptic::shutdown();
    if (hid::input_source() == st.source) hid::set_input_source(nullptr);
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        save_settings(st.path, st.settings, &err);
        st.dirty = false;
    }
    st.limiter.set_pre_frame_hook(nullptr, nullptr);
    st.started = false;
}

void open_settings_menu() {
    G& st = g();
    if (st.overlay) st.overlay->set_visible(true);
}

Stats stats() {
    G& st = g();
    Stats s;
    s.frames = st.frames.load();
    s.process_input_calls = st.pi_calls.load();
    s.aim_frames = st.aim_frames.load();
    s.events = st.events.load();
    s.mouse_counts_applied = st.counts.load();
    s.pump_thread_mismatch = st.thread_mismatch.load();
    return s;
}

}  // namespace nxrt::play
