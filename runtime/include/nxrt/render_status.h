// SPDX-License-Identifier: GPL-3.0-or-later
// The live render state the F1 page shows (Graphics > Live render state): which upscaler really ran, at which sizes, and whether frame
// generation is really inserting frames.
//
// Written where the work happens - src/gfx/nvn/api_dlss_replay.cpp for the upscaler, src/gfx/extras/framegen.cpp for frame generation,
// src/gfx/nvn/nvn_present.cpp for the backbuffer - and read by the menu. Those are not all the same thread (the DLSS evaluation runs on
// the render thread, the presents are counted on the present path), so every writer and the reader take one mutex: src/gfx/render_status.cpp.
// The numbers are the ones the passes used, not what the ini asks for: the input and output extents of the evaluation, and the presents
// Streamline or the FSR 3 swapchain actually counted. Nothing here allocates, touches the disk or writes the configuration back.
#pragma once
#include <cstdint>

namespace nxrt::render_status {

// The host upscaler of [dlss] upscaler. Auto is a choice, never a backend that runs.
enum class Backend : uint8_t { Unknown = 0, Auto, Taau, Bilinear, Fsr3, Dlss, Xess };
const char* backend_name(Backend b);   // "TAAU", "Bilinear", "FSR 3.1", "DLSS", "XeSS"; "Auto" / "unknown" for the other two
Backend backend_from_id(const char* id);   // "taau", "bilinear", "fsr3", "dlss", "xess", "auto"

// What frame generation is doing, from the backend's own lifecycle and its measured presents.
enum class FrameGen : uint8_t {
    Unknown = 0,    // nothing to measure: not started, or a backend this build cannot ask
    Off,            // [dlss] frame_gen = 0
    Waiting,        // asked for, the backend is not set up yet
    Ready,          // set up and switching on, no generated frame measured yet
    Generating,     // measured generated frames were presented in the last report window
    Paused,         // on, but the window held no new inputs (the pause reason says why)
    Failed,         // the backend refused to start
    Missing,        // a DLL, the swapchain or the GPU support is missing
};
const char* framegen_name(FrameGen fg);

// Why a window held no new inputs. Not every cause is a pause of frame generation (a backbuffer under the minimum turns it off, a frame shown
// again gives nothing to interpolate).
enum class Pause : uint8_t { None = 0, Vsync, NoInputs, SmallBackbuffer, Unfocused };
const char* pause_name(Pause p);

struct Snapshot {
    bool seen = false;               // an evaluation has run: the upscaler fields below are meaningful
    Backend configured = Backend::Unknown;
    Backend actual = Backend::Unknown;
    bool fallback = false;           // configured did not run, actual did
    char reason[128] = {};           // why it fell back, in the words the log used; empty when it did not
    int render_w = 0, render_h = 0;  // the evaluation's input extent
    int output_w = 0, output_h = 0;  // the evaluation's output extent
    int screen_w = 0, screen_h = 0;  // the backbuffer the picture is presented to
    int percent = 0;                 // render / output per side, rounded; 0 when unknown
    FrameGen fg = FrameGen::Unknown;
    bool fg_dlss = false, fg_ffx = false;   // the backend that ran (DLSS-G / FSR 3)
    Pause pause = Pause::None;
    uint32_t status = 0;             // DLSSGState::status as Streamline reports it
    // Presents in the last closed report window, as the backend counted them. DLSS-G's numFramesActuallyPresented and the FSR 3
    // swapchain count both cover every frame that reached the display, the game's own ones included, so shown_frames is not a
    // generated-frame count: generated is the difference the page shows.
    uint32_t shown_frames = 0;
    uint32_t game_frames = 0;        // the game's own frames in that window
    uint32_t generated = 0;          // shown_frames - game_frames, never below zero
    double game_fps = 0, shown_fps = 0;
    bool measured = false;           // a report window has closed (the counters mean something)
};

// Any thread. A copy: the caller may hold it across frames.
Snapshot snapshot();

// ---- writers (any thread: each call takes the mutex) -----------------------------------------------------------------------------
// The evaluation that just ran. Cheap to call every frame: it writes only when something changed or 200 ms passed.
void set_upscaler(Backend configured, Backend actual, int render_w, int render_h, int output_w, int output_h);
void set_fallback(bool fallback, const char* reason = "");   // the configured backend did not run: the reason is the log's own wording
void set_screen(int w, int h);
// shown_frames is what the backend presented (the game's frames included), game_frames the game's own count in the same window.
void set_framegen(FrameGen fg, bool dlss, bool ffx, Pause pause, uint32_t status, uint32_t shown_frames, uint32_t game_frames, double game_fps,
                  double shown_fps);

}  // namespace nxrt::render_status
