// SPDX-License-Identifier: GPL-3.0-or-later
// The NVN-on-Vulkan back end (S4d). install_backend() replaces the logging stubs of the NVN shell (nvn.h) with real
// implementations and starts the render thread that replays the game's command lists on a gfx::Device.
//   * Memory pools are the game's own memory: the back end imports it as Vulkan memory (VK_EXT_external_memory_host), so a GPU
//     address is a host address and buffers are zero-copy.
//   * Game threads only record. nvnQueueSubmitCommands hands the lists to the render thread and returns when they are recorded
//     into Vulkan (not when the GPU has run them); fences, nvnQueueFinish and the memory a game polls follow the GPU timeline.
//   * nvnQueuePresentTexture draws the window texture into the Renderer's frame (honouring the shell's FrameLimiter when one is
//     given) and presents it.
// Configuration (config.h, environment wins): [nvn] shader_cache (translated SPIR-V folder), bc7_cache (ASTC->BC7 cache folder),
// bc7_quality, log_unimplemented; see docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nxrt/gfx.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn {

struct BackendDesc {
    gfx::Device* device = nullptr;            // required; must outlive the back end
    gfx::Renderer* renderer = nullptr;        // optional: without it nvnQueuePresentTexture only waits for the GPU
    FrameLimiter* limiter = nullptr;          // optional: begin_frame() is called before each presented frame
    bool keep_aspect = false;                 // present with letterbox bars instead of stretching to the window
    bool register_entry_points = true;        // false for tests that call the Vulkan layer directly
};

bool install_backend(const BackendDesc& desc);   // idempotent per process: a second call with another device fails
void shutdown_backend();                         // waits for the GPU, destroys every Vulkan object of the back end
bool backend_installed();
// Writes the on-disk caches (the pipeline cache) now. For the paths that end the process without shutting the back end down (window closed).
void backend_flush_caches();
// Shows the game's last frame again (with the overlay on top) when the game has not presented for 50 ms and no present is queued. For while
// the game waits on the player in a library applet (the on-screen keyboard) and presents nothing. Any thread; does not wait.
void present_last_frame_again();

// Traces the next whole presented frame like [nvn] trace_frame (the scripted `@shot nvntrace_<name>` calls this).
void backend_trace_next_frame();

// The [nvn] shading_rate rules ("scene:2x2", "off"), from a settings menu; they replace the ini value until the process ends. Without
// the ini key at start (or VK_KHR_fragment_shading_rate) the pipelines have no dynamic shading rate and nothing changes.
void backend_set_shading_rate(const char* spec);

// [nvn] dynres: a frame-rate controller for a render scale (1 down to [nvn] dynres_min, 10% steps) that the game module applies:
// backend_dynres_enable turns it on with the function that makes the game render at `scale` of its configured size (called on the render
// thread, rarely: the game rebuilds its targets). backend_dynres_set: target frame rate (0 pauses), lowest scale and least gain per step
// from a settings menu (a negative value keeps the ini's). The scale in force in percent (0: off).
void backend_dynres_enable(void (*apply)(float scale));
void backend_dynres_set(double target_fps, double min_scale, double min_gain_pct);
void backend_dynres_on(bool on);   // after backend_dynres_enable: off puts the scale back to 1 at the next frame and stops the controller
// The upscalers' tuning values, live: the TAAU history weight (2..64) and the SDK upscalers' sharpening (0..1). A negative value keeps the
// one in force.
void backend_upscale_tuning(double history_weight, double sharpness);
int backend_dynres_scale_percent();

// The size and NVNmemoryPoolFlags of the initialized memory pool behind a guest NVNmemoryPool object. False when `guest_pool` is not a live
// pool of the back end. For game modules that look after a game's own pool allocators: a texture the game places past the end of a pool the
// CPU cannot access (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) gets GPU memory of its own instead of failing (nvnTextureInitialize).
bool memory_pool_info(const void* guest_pool, uint64_t* size, int* flags);

// Shader preparation (S24): compiles the shader parts of every program in a list before the game runs, so the driver already has them when the
// game asks. The list is written by the setup tool from the player's own game files (<shader cache folder>/../programs.bin, "MPRPRG01").
// Needs the back end installed (any mode; `register_entry_points = false` is enough) and an open shader cache.
struct ShaderPrepareOptions {
    std::string programs_file;                                    // empty: programs.bin next to the shader cache folder
    bool only_if_needed = false;                                  // do nothing when the state file says this GPU and driver were prepared already
    bool force = false;                                           // ignore the state file
    int threads = 0;                                              // 0: all hardware threads but two
    std::function<bool(uint64_t done, uint64_t total)> progress;  // called often; returning false cancels
};
struct ShaderPrepareResult {
    uint64_t programs = 0;          // in the list
    uint64_t prepared = 0;          // programs whose parts were all made
    uint64_t missing_shaders = 0;   // skipped: a stage has no translated shader in the cache
    uint64_t failed = 0;            // a part could not be made
    uint64_t parts = 0;
    uint64_t fast_parts = 0;        // parts made in under 2 ms: the driver already had them
    double seconds = 0;
    bool cancelled = false;
    bool already_ready = false;     // only_if_needed and nothing to do
    std::string error;              // why nothing could be done
};
bool prepare_shaders(const ShaderPrepareOptions& o, ShaderPrepareResult* r);

struct BackendStats {
    uint64_t submits = 0, lists_replayed = 0, draws = 0, draws_skipped = 0, dispatches = 0, clears = 0, copies = 0, presents = 0;
    uint64_t passes = 0, pass_breaks = 0, pipelines_created = 0, pipeline_cache_hits = 0, descriptor_sets = 0;
    uint64_t textures_created = 0, texture_uploads = 0, texture_upload_bytes = 0, bc7_transcodes = 0, textures_failed = 0;
    uint64_t pools_imported = 0, unresolved_addresses = 0, missing_handles = 0, unsupported_ops = 0;
    uint64_t submit_replay_ns = 0;
};
BackendStats backend_stats();
// Operations that were skipped because they are not implemented, with how often (for the log and docs/S4d.md).
std::string backend_unsupported_report();

}  // namespace nxrt::nvn
