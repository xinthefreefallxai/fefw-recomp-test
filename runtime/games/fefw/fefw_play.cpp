// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave, the PC features of play mode (fefw_game.h): the 60 fps option and the game clock log. Offsets are in main
// of update 131072 (build DB93B012...); the site is in recomp/games/fefw/main.sites. notes/pacing.md has the measurements.
//
// Frame rate. The game paces itself by the console's vsync. Its graphics object ([0x21e60d8]) holds the display rate (+0x1d8, 60), the
// target frame rate (+0x1dc) and the longest present interval (+0x1e0, 5); every frame is presented with interval min(+0x1e0, +0x1d8 /
// min(+0x1d8, +0x1dc)) (0x649920). The time manager (app::time::Manager, [0x22a6130]) steps the logic by 60 / target frames of 1/59.94 s,
// or by the measured time when a frame took more than that plus 0.1 (0x180eea0). Title and menus store target 60 (0x652960, 0x66af3c),
// gameplay stores 30 (0x7bf1c0, the 30 loaded at 0x7bf1ac). The runtime holds the game to 60 / interval (game::Info::present_interval_hz,
// gfx/nvn/nvn_present.cpp). [fefw] fps60 = 1 makes the gameplay store 60 (inline site 0x7bf1c0 sets w9), so gameplay runs at 60 fps and
// at the right speed, as the IPS patch of `mov w9, #30` does on the console.
//
// Game clock. [fefw] clock_log = N (seconds, default 10, 0 = off) logs how fast the game's clock ran against the host's over the last N
// seconds: the time manager's game time (its first step channel, +0x48, ticks) against its own real time (+0x18, ticks; both from
// nn::oe::GetProgramTotalActiveTime), with the target and the last step. 1.00x is the right speed. The clock is sampled every second
// whatever the period: the F1 menu (fefw_menu.cpp) shows it through clock_info() and changes the period live (set_clock_log).
//
// Map cursor. game::map::Cursor::Impl::Tick (0x735020, the battle map and world map cursor) moves the cursor each frame by the stick (dead
// zone 0.25) times 5 * 60 / target / a tuning value, twice that while Y is held (input flags bit 3): it counts the target frame rate, not the
// time the frame took, so whenever the game runs below its target (fps60 or fps120 not reached, a heavy scene, a slow PC) the cursor slows down
// with it while the game clock does not (#40). [fefw] cursor_real_time = 1 (default) puts the time manager's step of the frame (the same 60 /
// target when the frame was on time, the measured time when it was late, the smooth step with fps120) in place of 60 / target at the two sites
// (s0 after `fdiv s0, s1, s0` at 0x7358b4 and 0x735930), kept within 4 steps so a load does not throw the cursor across the map.
//
// Movies (media/movie_vdec.cpp, on when the host movie::VideoDecoder is). The player's setup (0x1906510) takes its frame path from the byte
// at +0x1318 (`ldrb w21` at 0x190654c): 0 native images, the console's, which the host cannot fill; 1 frames in CPU buffers, which the host
// decoder writes. The site at 0x1906550 makes w21 1. On that path the player sets its frame count (+0xff8) to 4 only after it made the
// buffers (2 by default, 0x1903ae0), then registers 4 and the third is missing: the site at 0x1906634 sets the count first. nxnative-penis6's
// patches.py makes the same two changes in the code.
#include "fefw_game.h"
#include "play_types.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include <mutex>
#include <thread>

#include <windows.h>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/movie_vdec.h"
#include "nxrt/pgo.h"
#include "nxrt/tt.h"

// Profile-guided builds of the lifted modules (notes/perf.md, docs/PGO.md): a test run ends with TerminateProcess, so when the profile runtime
// is linked its exit writes the counters first (nxrt::profile_flush, runtime/src/core/pgo.cpp, which also holds the stub for other builds).

namespace nxrt::games::fefw {

void twprobe_start(uint64_t base);      // fefw_twprobe.cpp (debug)

namespace {

constexpr const char* kTag = "fefw";

constexpr uint64_t kGraphics = 0x21e60d8;          // [ptr] the graphics object
constexpr uint64_t kTargetFps = 0x1dc;             // s32 in it: the target frame rate
constexpr uint64_t kDisplayRate = 0x1d8;           // s32 in it: the display rate the present interval counts in (60)
constexpr uint64_t kMaxInterval = 0x1e0;           // s32 in it: the longest present interval (5)
constexpr uint64_t kGameplayTargetStore = 0x7bf1c0;  // inline: `str w9, [x8, #0x1dc]`, w9 = 30
constexpr uint64_t kTimeManager = 0x22a6130;       // [ptr] app::time::Manager
constexpr uint64_t kTmReal = 0x18;                 // s64: real time, ticks
constexpr uint64_t kTmGame = 0x48;                 // s64: game time of step channel 0, ticks
constexpr uint64_t kTmStep = 0x44;                 // f32: the last step, frames of 1/59.94 s
constexpr uint64_t kStepChoice = 0x180ef44;        // inline in the step (0x180eea0), before its clamp: s0 measured, s1 the chosen step
constexpr uint64_t kCursorStepX = 0x7358b8;        // inline in game::map::Cursor::Impl::Tick: s0 = 60 / target (x movement)
constexpr uint64_t kCursorStepY = 0x735934;        // the same for the other axis
constexpr uint64_t kMovieSelect = 0x1906550;       // inline: after `ldrb w21, [x19, x8]`, the player's frame path (+0x1318)
constexpr uint64_t kMovieCount = 0x1906634;        // inline: after the frame buffer object's init, before its buffers are made (0x1906640)
constexpr uint64_t kMovieStorage = 0xc10;          // s32 in the player: 1 = frames in CPU buffers
constexpr uint64_t kMovieFrames = 0xff8;           // s64 in the player: the frame buffers to make on that path

struct State {
    uint64_t base = 0x7100000000ull;
    std::atomic<int> rate{30};      // the gameplay frame rate target: 30 (the game's own), 60 or 120. The menu changes it (play_set_rate)
    std::atomic<bool> rate_thread{false};
    uint32_t hook = 0, movie_select = 0, movie_count = 0, step_hook = 0, cursor_x = 0, cursor_y = 0;
    std::atomic<uint64_t> cursor_fixes{0};
    std::atomic<uint64_t> cursor_ratio_milli{0};   // sum of used / game step, x1000, since the last clock log line
    std::atomic<bool> smooth{false};   // the menu changes it (play_set_smooth)
    int smooth_mode = -1;              // [fefw] smooth_step: -1 auto (on above 30), 0 off, 1 on
    FILE* step_csv = nullptr;      // [fefw] step_csv: per frame host ms, target, measured, the game's step, the step used (frames)
    uint64_t steps_logged = 0;
    float step_debt = 0.0f;         // smooth_step: measured minus stepped time, frames
    std::atomic<uint64_t> steps{0};
    std::atomic<uint64_t> movies{0};
    std::atomic<uint64_t> stores{0};
    std::atomic<bool> clock_run{false};
};

State& st() {
    static State s;
    return s;
}

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}

// fps120: the display rate becomes 120 with the target, so menus (target 60) keep interval 2 = 60 fps and the game's step is 60 / 120.
void raise_display_rate(uint64_t gfx) {
    int32_t rate = 0, longest = 0;
    if (!rd(gfx + kDisplayRate, &rate) || !rd(gfx + kMaxInterval, &longest) || rate <= 0 || rate == 120) return;
    const int32_t r = 120, l = longest * 120 / rate;
    std::memcpy(reinterpret_cast<void*>(gfx + kMaxInterval), &l, 4);
    std::memcpy(reinterpret_cast<void*>(gfx + kDisplayRate), &r, 4);
    NXLOG_I(kTag, "display rate %d -> 120, longest present interval %d -> %d (fps120)", rate, longest, l);
}

// The site is hooked whatever the setting, so the menu can change the rate while the game runs: at 30 the game's own store goes through.
void hook_gameplay_target(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const int rate = s.rate.load(std::memory_order_relaxed);
    s.stores.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> logged{0};
    if (logged.exchange(rate) != rate) NXLOG_I(kTag, "gameplay frame rate target: %d", rate);
    if (rate == 30) return;
    if (rate == 120) {
        raise_display_rate(c->x[8]);
        set_present_interval_hz(120);
    }
    c->x[9] = static_cast<uint64_t>(rate);
}

// smooth_step: the game steps 60 / target unless the frame took that plus 0.1, else the measured time. Every shorter frame counts a whole
// step and frames up to 0.1 over count none of the excess: at 120 (0.1 = 20 % of a step) the clock runs ~7 % slow near 110 fps, at 60 it
// runs fast when the main thread's frames are uneven and slow when they are 1.0-1.1 steps. The step here is the game's plus a quarter of
// the time it still owes (measured minus stepped, kept within 4 steps): the game's own even steps where they are right, never a lasting
// error. Frames over 8 steps (loads, stalls) keep the game's rule and clear the debt.
void hook_step_choice(::Ctx* c, uint64_t, void*) {
    State& s = st();
    uint64_t tm = 0, gfx = 0;
    int32_t target = 0;
    if (!rd(s.base + kTimeManager, &tm) || c->x[19] != tm) return;
    if (!rd(s.base + kGraphics, &gfx) || !rd(gfx + kTargetFps, &target) || target <= 0) return;
    const float period = 60.0f / static_cast<float>(target), e = c->v[0].f[0], chosen = c->v[1].f[0];
    if (s.smooth.load(std::memory_order_relaxed)) {
        if (!(e > 0.0f) || e > 8.0f * period) {
            s.step_debt = 0.0f;
        } else {
            const float u = std::max(0.25f * period, chosen + 0.25f * (s.step_debt + e - chosen));
            s.step_debt = std::clamp(s.step_debt + e - u, -4.0f * period, 4.0f * period);
            c->v[1].f[0] = u;
            if (s.steps.fetch_add(1, std::memory_order_relaxed) == 0)
                NXLOG_I(kTag, "time step: the game's step plus a quarter of the time it owes (smooth_step)");
        }
    }
    if (s.step_csv) {
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        std::fprintf(s.step_csv, "%.3f,%d,%.4f,%.4f,%.4f\n", static_cast<double>(q.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart),
                     target, e, chosen, c->v[1].f[0]);
        if ((s.steps_logged++ & 63) == 0) std::fflush(s.step_csv);
    }
}

// [fefw] cursor_real_time: the frame's step (time manager +0x44, frames of 1/59.94 s) for 60 / target, within (0, 4 * 60 / target].
void hook_cursor_step(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const float nominal = c->v[0].f[0];
    uint64_t tm = 0;
    float step = 0;
    if (!(nominal > 0.0f) || !rd(s.base + kTimeManager, &tm) || !rd(tm + kTmStep, &step) || !(step > 0.0f)) return;
    const float used = std::min(step, 4.0f * nominal);
    c->v[0].f[0] = used;
    s.cursor_ratio_milli.fetch_add(static_cast<uint64_t>(used / nominal * 1000.0f), std::memory_order_relaxed);
    s.cursor_fixes.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        NXLOG_I(kTag, "map cursor: moves by the frame's real step (%.2f frames; the game's 60 / target is %.2f)", static_cast<double>(step),
                static_cast<double>(nominal));
}

void hook_movie_select(::Ctx* c, uint64_t, void*) {
    c->x[21] = 1;
    if (st().movies.fetch_add(1, std::memory_order_relaxed) == 0) NXLOG_I(kTag, "movie player: frames in CPU buffers (host decoder)");
}

void hook_movie_count(::Ctx* c, uint64_t, void*) {
    const uint64_t player = c->x[19];
    int32_t storage = 0;
    if (!rd(player + kMovieStorage, &storage) || storage != 1 || !mem::is_mapped(player + kMovieFrames, 8)) return;
    const uint64_t frames = 4;
    std::memcpy(reinterpret_cast<void*>(player + kMovieFrames), &frames, 8);
}

// Samples the clock once a second: the F1 menu shows the rate over the last few seconds (clock_info), the log gets a line every
// clock_log seconds (set_clock_log changes it live; 0 = no lines).
std::atomic<int> g_clock_period{10};
std::mutex g_clock_mu;
ClockInfo g_clock;

void clock_thread() {
    State& s = st();
    constexpr int kWin = 5;                  // seconds the menu's rate covers
    int64_t real_w[kWin + 1] = {}, game_w[kWin + 1] = {};
    int nw = 0;
    int64_t real0 = 0, game0 = 0;
    int since_log = 0;
    bool have = false;
    while (s.clock_run.load(std::memory_order_relaxed)) {
        Sleep(1000);
        uint64_t tm = 0, gfx = 0;
        int64_t real = 0, game = 0;
        float step = 0;
        int32_t target = 0, display = 0;
        if (!rd(s.base + kTimeManager, &tm) || !rd(tm + kTmReal, &real) || !rd(tm + kTmGame, &game) || !rd(tm + kTmStep, &step)) {
            have = false;
            nw = 0;
            std::lock_guard<std::mutex> lk(g_clock_mu);
            g_clock.valid = false;
            continue;
        }
        if (rd(s.base + kGraphics, &gfx)) {
            rd(gfx + kTargetFps, &target);
            rd(gfx + kDisplayRate, &display);
        }
        if (nw == kWin + 1) {
            std::memmove(real_w, real_w + 1, sizeof real_w - sizeof real_w[0]);
            std::memmove(game_w, game_w + 1, sizeof game_w - sizeof game_w[0]);
            --nw;
        }
        real_w[nw] = real;
        game_w[nw] = game;
        ++nw;
        {
            std::lock_guard<std::mutex> lk(g_clock_mu);
            g_clock.valid = nw >= 2 && real_w[nw - 1] > real_w[0];
            g_clock.rate = g_clock.valid ? static_cast<double>(game_w[nw - 1] - game_w[0]) / static_cast<double>(real_w[nw - 1] - real_w[0]) : 0;
            g_clock.window_s = nw - 1;
            g_clock.target = target;
            g_clock.display = display;
            g_clock.step = step;
        }
        const int period_s = g_clock_period.load(std::memory_order_relaxed);
        if (period_s <= 0) {
            have = false;
            continue;
        }
        if (have && ++since_log < period_s) continue;
        if (have && real > real0) {
            const double rate = static_cast<double>(game - game0) / static_cast<double>(real - real0);
            NXLOG_I(kTag, "game clock: %.3fx real time over %d s (target %d fps, display %d, last step %.2f frames)", rate, period_s,
                    target, display, static_cast<double>(step));
            // the map cursor (cursor_real_time): how much faster it moved than the game's own 60 / target would have made it
            const uint64_t n = s.cursor_fixes.exchange(0), sum = s.cursor_ratio_milli.exchange(0);
            if (n > 1)
                NXLOG_I(kTag, "map cursor: %llu moves (2 per frame), real step / the game's 60 / target %.2f on average", static_cast<unsigned long long>(n),
                        static_cast<double>(sum) / static_cast<double>(n) / 1000.0);
        }
        real0 = real;
        game0 = game;
        since_log = 0;
        have = true;
    }
}

// fps120: the runtime's rate follows the game's display rate should the game ever store it again.
void rate_thread() {
    State& s = st();
    while (s.clock_run.load(std::memory_order_relaxed)) {
        Sleep(100);
        uint64_t gfx = 0;
        int32_t display = 0;
        if (!rd(s.base + kGraphics, &gfx) || !rd(gfx + kDisplayRate, &display) || display <= 0) continue;
        const double hz = present_interval_hz_override();
        if (hz > 0 && hz != display) {
            NXLOG_W(kTag, "the game's display rate is now %d: the runtime follows (fps120)", display);
            set_present_interval_hz(display);
        }
    }
}

void exit_writing_profile(int code) {
    profile_flush();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
}

}  // namespace

void play_install(uint64_t base, play::GameHost*, const play::Settings&) {
    State& s = st();
    if (profile_runtime_linked()) tt::set_exit_handler(&exit_writing_profile);
    s.base = base;
    res_install(base);
    twprobe_start(base);
    const bool fps60 = config_get_bool("fefw", "fps60", false), fps120 = config_get_bool("fefw", "fps120", false);
    s.rate.store(fps120 ? 120 : fps60 ? 60 : 30);
    s.hook = hook_add_inline(base + kGameplayTargetStore, &hook_gameplay_target);
    // The game only runs at its target when the frame limit lets that many frames through: the shipped [display] fps_limit = 60 held fps120 at
    // 60 without a word in the log. The floor raises a numeric limit below the target (play mode logs it); "auto" and vsync keep the display's say
    // and get a warning there.
    if (fps120) set_frame_cap_floor(120);
    else if (fps60) set_frame_cap_floor(60);
    // smooth_step defaults on with fps60 and fps120 (issue 24: the game's own rule runs 0.2-1.5 % fast at 60 when frames are uneven)
    {
        std::string m = config_get_str("fefw", "smooth_step", "auto");
        s.smooth_mode = m.empty() || m == "auto" || m == "Auto" ? -1 : config_get_bool("fefw", "smooth_step", fps120 || fps60) ? 1 : 0;
    }
    const bool smooth = s.smooth_mode < 0 ? fps120 || fps60 : s.smooth_mode == 1;
    s.smooth.store(smooth);
    const std::string step_csv = config_get_str("fefw", "step_csv", "");
    if (!step_csv.empty()) s.step_csv = std::fopen(step_csv.c_str(), "w");
    if (s.step_csv) {
        std::fprintf(s.step_csv, "host_ms,target,measured,game_step,used_step\n");
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        NXLOG_I(kTag, "step csv %s: host_ms %.3f at this line", step_csv.c_str(),
                static_cast<double>(q.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
    }
    s.step_hook = hook_add_inline(base + kStepChoice, &hook_step_choice);   // always: the menu switches smooth_step while the game runs
    if (config_get_bool("fefw", "cursor_real_time", true)) {
        s.cursor_x = hook_add_inline(base + kCursorStepX, &hook_cursor_step);
        s.cursor_y = hook_add_inline(base + kCursorStepY, &hook_cursor_step);
    }
    if (media::movie_decoder_enabled()) {
        s.movie_select = hook_add_inline(base + kMovieSelect, &hook_movie_select);
        s.movie_count = hook_add_inline(base + kMovieCount, &hook_movie_count);
    }
    const int64_t period = config_get_int("fefw", "clock_log", 10);
    g_clock_period.store(static_cast<int>(std::clamp<int64_t>(period, 0, 3600)));
    const bool threads = !s.clock_run.exchange(true);   // the clock thread always samples (the F1 menu shows it); it logs when period > 0
    if (threads) std::thread(clock_thread).detach();
    if (threads && fps120 && !s.rate_thread.exchange(true)) std::thread(rate_thread).detach();
    NXLOG_I(kTag, "gameplay at %s fps ([fefw] fps60 = %d, fps120 = %d, smooth_step = %d), game clock log every %lld s",
            fps120 ? "120" : fps60 ? "60" : "30 (the game's own)", fps60 ? 1 : 0, fps120 ? 1 : 0, smooth ? 1 : 0,
            static_cast<long long>(period));
}

// The menu changed the gameplay frame rate (main thread). The game stores its target when a gameplay scene starts (the hook above), so
// the new rate holds from the next one. When the target in force is plainly gameplay's (the old rate, and not 60, which menus also
// store), it is written at once.
void play_set_rate(int fps) {
    State& s = st();
    fps = fps >= 120 ? 120 : fps >= 60 ? 60 : 30;
    const int old = s.rate.exchange(fps);
    if (s.smooth_mode < 0) s.smooth.store(fps != 30);
    set_frame_cap_floor(fps == 30 ? 0 : fps);
    if (fps == 120 && s.clock_run.load() && !s.rate_thread.exchange(true)) std::thread(rate_thread).detach();
    if (old == fps) return;
    uint64_t gfx = 0;
    int32_t target = 0;
    bool now = false;
    if (rd(s.base + kGraphics, &gfx) && rd(gfx + kTargetFps, &target) && s.stores.load() > 0 && target == old && old != 60) {
        if (fps == 120) {
            raise_display_rate(gfx);
            set_present_interval_hz(120);
        }
        const int32_t v = fps;
        std::memcpy(reinterpret_cast<void*>(gfx + kTargetFps), &v, 4);
        now = true;
    }
    NXLOG_I(kTag, "[live] gameplay frame rate %d -> %d: %s (the game's target was %d), smooth_step = %d", old, fps,
            now ? "in force now" : "from the next gameplay scene", target, s.smooth.load() ? 1 : 0);
}

// The menu changed [fefw] smooth_step: -1 auto (on above 30 fps), 0 off, 1 on.
void play_set_smooth(int mode) {
    State& s = st();
    s.smooth_mode = mode < 0 ? -1 : mode ? 1 : 0;
    const bool on = s.smooth_mode < 0 ? s.rate.load() != 30 : s.smooth_mode == 1;
    s.smooth.store(on);
    NXLOG_I(kTag, "[live] smooth_step %s", on ? "on" : "off");
}

ClockInfo clock_info() {
    ClockInfo c;
    {
        std::lock_guard<std::mutex> lk(g_clock_mu);
        c = g_clock;
    }
    const State& s = st();
    c.period_s = g_clock_period.load(std::memory_order_relaxed);
    c.fps60 = s.rate.load() == 60;
    c.fps120 = s.rate.load() == 120;
    c.smooth = s.smooth.load();
    return c;
}

void set_clock_log(int period_s) {
    g_clock_period.store(std::clamp(period_s, 0, 3600), std::memory_order_relaxed);
    NXLOG_I(kTag, "game clock log every %d s%s", period_s, period_s > 0 ? "" : " (off)");
}

void play_remove() {
    State& s = st();
    wide_remove();
    for (uint32_t* h : {&s.hook, &s.movie_select, &s.movie_count, &s.step_hook, &s.cursor_x, &s.cursor_y}) {
        if (*h) hook_remove(*h);
        *h = 0;
    }
    set_present_interval_hz(0);
    set_frame_cap_floor(0);
    s.clock_run.store(false);
}

}  // namespace nxrt::games::fefw
