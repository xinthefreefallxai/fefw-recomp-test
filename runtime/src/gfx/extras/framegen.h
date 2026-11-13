// SPDX-License-Identifier: GPL-3.0-or-later
// DLSS Frame Generation through Streamline ([dlss] frame_gen = 1, notes/framegen.md): the game renders and paces as before; Streamline's
// swapchain hooks add one generated frame per presented frame. Reflex and PCL markers (which DLSS-G requires) come from the render thread:
// the whole frame (DLSS evaluation, present) is built there, so one frame index serves the constants, the tags and the markers.
#pragma once
#include <volk.h>

#include <cstdint>

namespace nxrt::extras {

// [dlss] frame_gen (read once): 0 off; 1 (or dlss) DLSS-G, Streamline then also loads DLSS-G, Reflex and PCL; 2 DLSS-G loaded but kept
// off (A/B test aid); 3 (or fsr3) FSR 3 frame generation (framegen_ffx.h)
int framegen_mode();
bool framegen_requested();   // mode != 0
bool framegen_dlss();        // mode 1 or 2: Streamline is needed
// [dlss] frame_gen_any_gpu (default off, read once): Streamline's support check for DLSS-G (slIsFeatureSupported) answering "adapter" or
// "feature not supported" (GPU generation) is logged, not obeyed, so player-supplied DLLs that make DLSS-G run on older RTX GPUs (e.g. an
// RTX 30 mod) are not refused by us; the DLSS-G plugin's own state (slDLSSGGetState: at least one generated frame) still decides. True when
// `sl_result` (an sl::Result) is to be let through.
bool framegen_support_override(int sl_result);
bool framegen_running();     // the chosen backend works (DLSS-G supported, or the FSR 3 swapchain made)
uint64_t framegen_take_sleep_ns();   // time spent in the Reflex sleep (DLSS-G) since the last call

// The present's pacing with frame generation (render thread, before the frame limiter): game_fps is the game's own rate (0: none), fifo the
// swapchain presents with vsync, refresh_hz the display's rate (0: unknown). Two frames are shown per rendered one, and with vsync both have
// to fit the display: the rendered rate is then held just under half the refresh rate (AMD's FSR 3 pacing guidance; at the full refresh rate
// the presents queue behind the display and the render thread, which also replays the game, waits on them: the game then ran slower with
// frame generation than without it). Where half the refresh rate is well below the game's own rate (a 60 Hz display with 60 fps
// gameplay), generation pauses instead and the game keeps its rate. Returns the rate for the limiter.
double framegen_pace(double game_fps, bool fifo, float refresh_hz);
bool framegen_vsync_paused();   // generation is paused by that rule (F1 page)

// The Streamline frame token for DLSS SR's constants (viewport 0). Without frame_gen: a new token per call, as before. With it: the frame
// the render thread is building (its index advances at each present; a second evaluation before the present takes the next index, since
// Streamline refuses two sets of constants in one frame). Null without Streamline.
void* sl_frame_token();

// The frame's depth (r32f) and motion vectors (rg16f, render UV units, previous minus current) of the rendered region, GENERAL layout, in
// the game's orientation (flip_y: the window is presented upside down, so these are flipped for the backbuffer). Render thread, from the
// DLSS evaluation; they are copied, so the caller may reuse its images.
struct FramegenInputs {
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;
    VkExtent2D size = {};
    float jitter_px[2] = {};
    bool reset = false, depth_inverted = true, flip_y = false;
};
void framegen_inputs(void* gfx_device, VkCommandBuffer cb, const FramegenInputs& in);

// The scene without the UI at the backbuffer's size and format (GENERAL layout), drawn as the present draws (nvn_framegen.cpp). Render
// thread, before framegen_present_begin; tagged only when the frame's depth and motion were.
void framegen_hudless(VkCommandBuffer cb, VkImage image, VkImageView view, VkFormat format, VkExtent2D size);

// The present (render thread): before Renderer::end_frame and after it. The picture is the crop (x, y, w, h; empty: all) of a `src` large
// image, letterboxed into the backbuffer with keep_aspect, as the present passes draw it. again: the game's last frame shown once more (no
// new inputs: generation pauses).
// cb: the present's command buffer (still recording).
void framegen_present_begin(VkCommandBuffer cb, VkExtent2D backbuffer, VkExtent2D src, const int32_t crop[4], bool keep_aspect, bool again);
void framegen_present_end();
void framegen_swapchain_reset();   // renderer is idle, before replacing/destroying a swapchain
void framegen_shutdown(void* gfx_device);
// Shown frames per game frame over the last 2 s report (DLSS-G or FSR 3), 0 while generation was on for less than half of them: the
// frame rate counter multiplies the game's frame rate by it. Any thread.
float framegen_shown_ratio();

// sl.interposer.dll's export `name` (temporal_dlss.cpp); null when Streamline was not started.
void* streamline_proc(const char* name);

}  // namespace nxrt::extras
