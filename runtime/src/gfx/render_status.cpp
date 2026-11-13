// SPDX-License-Identifier: GPL-3.0-or-later
// The live render state (render_status.h). One mutex around the struct, because there is more than one writer: the DLSS evaluation
// publishes from the render thread (src/gfx/nvn/api_dlss_replay.cpp), frame generation from the present path
// (src/gfx/extras/framegen.cpp), and the backbuffer extent from src/gfx/nvn/nvn_present.cpp. The readers are the F1 page, which takes a
// copy, and the once-a-second log line. The critical sections are a struct copy each, and the write paths call in at most four times a
// second, so a mutex costs nothing measurable here and keeps the code free of a hand-rolled lock-free protocol.
#include "nxrt/render_status.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace nxrt::render_status {
namespace {

std::mutex g_mu;
Snapshot g_snap;

// The caller already holds g_mu: read the current state, change it, write it back. Every setter below follows this shape.
Snapshot& writable() { return g_snap; }

}  // namespace

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::Taau: return "TAAU";
        case Backend::Bilinear: return "Bilinear";
        case Backend::Fsr3: return "FSR 3.1";
        case Backend::Dlss: return "DLSS";
        case Backend::Xess: return "XeSS";
        case Backend::Auto: return "Auto";
        default: return "unknown";
    }
}

Backend backend_from_id(const char* id) {
    if (!id) return Backend::Unknown;
    if (!std::strcmp(id, "taau") || !std::strcmp(id, "spatial_taau")) return Backend::Taau;
    if (!std::strcmp(id, "bilinear") || !std::strcmp(id, "spatial")) return Backend::Bilinear;
    if (!std::strcmp(id, "fsr3") || !std::strcmp(id, "fsr")) return Backend::Fsr3;
    if (!std::strcmp(id, "dlss") || !std::strcmp(id, "dlaa")) return Backend::Dlss;
    if (!std::strcmp(id, "xess")) return Backend::Xess;
    if (!std::strcmp(id, "auto")) return Backend::Auto;
    return Backend::Unknown;
}

const char* framegen_name(FrameGen fg) {
    switch (fg) {
        case FrameGen::Off: return "off";
        case FrameGen::Waiting: return "waiting for the backend";
        case FrameGen::Ready: return "set up, no generated frame measured yet";
        case FrameGen::Generating: return "generating frames";
        case FrameGen::Paused: return "paused";
        case FrameGen::Failed: return "failed to start";
        case FrameGen::Missing: return "unavailable (DLLs, swapchain or GPU support)";
        default: return "unknown";
    }
}

const char* pause_name(Pause p) {
    switch (p) {
        case Pause::Vsync: return "vsync is on and the display cannot show two frames per game frame";
        case Pause::NoInputs: return "the window had no new inputs (the frame was shown again)";
        case Pause::SmallBackbuffer: return "the backbuffer is under the minimum size";
        case Pause::Unfocused: return "the game window does not have the focus";
        default: return "";
    }
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_snap;   // a copy: the caller keeps it across frames
}

void set_upscaler(Backend configured, Backend actual, int render_w, int render_h, int output_w, int output_h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Snapshot& s = writable();
    if (s.seen && s.configured == configured && s.actual == actual && s.render_w == render_w && s.render_h == render_h && s.output_w == output_w &&
        s.output_h == output_h)
        return;
    s.seen = true;
    s.configured = configured;
    s.actual = actual;
    s.fallback = configured != Backend::Auto && configured != actual;
    if (!s.fallback) s.reason[0] = '\0';   // the backend recovered: the old wording would be a lie
    s.render_w = render_w;
    s.render_h = render_h;
    s.output_w = output_w;
    s.output_h = output_h;
    s.percent = output_w > 0 ? static_cast<int>(render_w * 100.0 / output_w + 0.5) : 0;
}

void set_fallback(bool fallback, const char* reason) {
    std::lock_guard<std::mutex> lk(g_mu);
    Snapshot& s = writable();
    if (s.fallback == fallback && !std::strcmp(s.reason, reason ? reason : "")) return;
    s.fallback = fallback;
    std::snprintf(s.reason, sizeof(s.reason), "%s", reason ? reason : "");
}

void set_screen(int w, int h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Snapshot& s = writable();
    s.screen_w = w;
    s.screen_h = h;
}

void set_framegen(FrameGen fg, bool dlss, bool ffx, Pause pause, uint32_t status, uint32_t shown_frames, uint32_t game_frames, double game_fps,
                  double shown_fps) {
    std::lock_guard<std::mutex> lk(g_mu);
    Snapshot& s = writable();
    s.fg = fg;
    s.fg_dlss = dlss;
    s.fg_ffx = ffx;
    s.pause = pause;
    s.status = status;
    // The backend counts every frame it presented, the game's own ones included: only the difference is a generated frame.
    s.shown_frames = shown_frames;
    s.game_frames = game_frames;
    s.generated = shown_frames > game_frames ? shown_frames - game_frames : 0;
    s.game_fps = game_fps;
    s.shown_fps = shown_fps;
    s.measured = s.measured || game_frames > 0;
}

}  // namespace nxrt::render_status
