// SPDX-License-Identifier: GPL-3.0-or-later
// Frame generation (framegen.h, notes/framegen.md): DLSS-G through Streamline, or FSR 3 (framegen_ffx.cpp).
#include "extras/framegen.h"
#include "extras/framegen_ffx.h"
#include "extras/temporal.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "nxrt/render_status.h"

#include "../../../third_party/nvidia_streamline/include/sl.h"
#include "../../../third_party/nvidia_streamline/include/sl_consts.h"
#include "../../../third_party/nvidia_streamline/include/sl_dlss_g.h"
#include "../../../third_party/nvidia_streamline/include/sl_pcl.h"
#include "../../../third_party/nvidia_streamline/include/sl_reflex.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "framegen";

// DLSS-G reads viewport 0 at present (ProgrammingGuideDLSS_G, tagging checklist).
// SR uses a separate viewport so its unflipped inputs/constants cannot replace ours.

int fg_setting() {
    static const int v = [] {
        std::string m = config_get_str("dlss", "frame_gen", "0");
        for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "1" || m == "dlss" || m == "dlssg" || m == "on") return 1;
        if (m == "2") return 2;
        if (m == "3" || m == "fsr3" || m == "fsr") return 3;
        if (m != "0" && m != "off" && !m.empty()) NXLOG_W(kTag, "[dlss] frame_gen = %s is not known: off", m.c_str());
        return 0;
    }();
    return v;
}
bool dlss_fg() { return fg_setting() == 1 || fg_setting() == 2; }

struct Fns {
    PFun_slGetNewFrameToken* token = nullptr;
    PFun_slSetConstants* constants = nullptr;
    PFun_slSetTagForFrame* tag = nullptr;
    PFun_slGetFeatureFunction* feature_fn = nullptr;
    PFun_slIsFeatureSupported* supported = nullptr;
    PFun_slDLSSGSetOptions* fg_set = nullptr;
    PFun_slDLSSGGetState* fg_get = nullptr;
    PFun_slReflexSetOptions* reflex_set = nullptr;
    PFun_slReflexSleep* reflex_sleep = nullptr;
    PFun_slReflexGetState* reflex_get = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
};

struct Pair {
    gfx::Image depth, motion;
    bool init = false;
};

struct State {
    bool tried = false, base = false;   // base: the core functions (frame tokens)
    bool markers = false;               // Reflex and PCL loaded
    bool fg_tried = false;
    bool fg = false;                    // DLSS-G usable
    Fns f;
    gfx::Device* dev = nullptr;
    uint32_t index = 1;
    uint32_t sr_index = 0;              // the index DLSS SR last set constants for
    bool open = false;
    bool inputs = false;                // this frame's depth and motion were tagged
    int streak = 0;
    bool on = false;
    int32_t rect[4] = {};
    VkExtent2D bb = {};
    Pair pair[2];
    uint32_t cur = 0;
    std::vector<std::pair<uint32_t, gfx::Image>> retired;
    uint64_t t0 = 0;
    uint32_t n_game = 0, n_shown = 0, n_on = 0, n_extra_inputs = 0, n_extra_sr = 0;
    uint32_t status = 0;
    uint32_t min_dimension = 0;
    FfxFgFrame ffx;                     // FSR 3: this frame's inputs, used at the present
    VkImage ffx_hudless = VK_NULL_HANDLE;
    uint64_t last_present_ns = 0, ffx_count0 = 0;
    bool vsync_paused = false, pace_told = false;   // framegen_pace
    bool small_bb = false;                          // the backbuffer was under the minimum on the last present
    render_status::Pause pause_reason = render_status::Pause::None;   // why the last present could not generate
    // The last closed report window, for the F1 page (render_status.h). win_shown is every frame the backend presented, the game's own
    // ones included (numFramesActuallyPresented, or the FSR 3 swapchain count), so it is not a generated-frame count: the page subtracts
    // win_game. A window in which the backend presented no more frames than the game itself made proves nothing: the page says "set up",
    // not "generating".
    uint32_t win_shown = 0, win_game = 0;
    double win_game_fps = 0, win_shown_fps = 0;
    bool win_measured = false;
    uint64_t status_ns = 0;                         // the last publish_status (250 ms throttle)
};
State& st() {
    static State s;
    return s;
}
std::atomic<int> g_api_error{0};
std::atomic<uint64_t> g_sleep_ns{0};   // the last Reflex sleep (DLSS-G), for [nvn] frame_csv
std::atomic<float> g_shown_ratio{0.0f};   // framegen_shown_ratio
void set_shown_ratio(double game, double shown, uint32_t on) {
    const bool mostly_on = game > 0 && on * 2 >= game;
    g_shown_ratio.store(mostly_on ? static_cast<float>(shown / game) : 0.0f, std::memory_order_relaxed);
}
// Generation stopped for good (no more reports): the counter goes back to the game's own rate.
void clear_shown_ratio() { g_shown_ratio.store(0.0f, std::memory_order_relaxed); }

void on_api_error(const sl::APIError& e) { g_api_error.store(static_cast<int>(e.vkRes), std::memory_order_relaxed); }

template <class T>
bool feature_fn(State& s, sl::Feature feat, const char* name, T*& out) {
    void* p = nullptr;
    if (s.f.feature_fn(feat, name, p) != sl::Result::eOk || !p) {
        NXLOG_W(kTag, "Streamline has no %s", name);
        return false;
    }
    out = reinterpret_cast<T*>(p);
    return true;
}

bool base_init() {
    State& s = st();
    if (s.tried) return s.base;
    s.tried = true;
    s.f.token = reinterpret_cast<PFun_slGetNewFrameToken*>(streamline_proc("slGetNewFrameToken"));
    s.f.constants = reinterpret_cast<PFun_slSetConstants*>(streamline_proc("slSetConstants"));
    s.f.tag = reinterpret_cast<PFun_slSetTagForFrame*>(streamline_proc("slSetTagForFrame"));
    s.f.feature_fn = reinterpret_cast<PFun_slGetFeatureFunction*>(streamline_proc("slGetFeatureFunction"));
    s.f.supported = reinterpret_cast<PFun_slIsFeatureSupported*>(streamline_proc("slIsFeatureSupported"));
    s.base = s.f.token && s.f.constants && s.f.tag && s.f.feature_fn && s.f.supported;
    if (!s.base || !dlss_fg() || !streamline_framegen_available()) return s.base;
    if (feature_fn(s, sl::kFeatureReflex, "slReflexSetOptions", s.f.reflex_set) && feature_fn(s, sl::kFeatureReflex, "slReflexSleep", s.f.reflex_sleep) &&
        feature_fn(s, sl::kFeatureReflex, "slReflexGetState", s.f.reflex_get) && feature_fn(s, sl::kFeaturePCL, "slPCLSetMarker", s.f.marker)) {
        sl::ReflexOptions ro;
        ro.mode = sl::ReflexMode::eLowLatency;
        ro.frameLimitUs = 0;   // the runtime's limiter paces the game
        const sl::Result r = s.f.reflex_set(ro);
        s.markers = r == sl::Result::eOk;
        if (!s.markers) NXLOG_W(kTag, "slReflexSetOptions failed (%d): frame generation off", static_cast<int>(r));
        else NXLOG_I(kTag, "Reflex on (low latency), PCL markers from the render thread");
    }
    return s.base;
}

void marker(State& s, sl::PCLMarker m) {
    sl::FrameToken* t = nullptr;
    if (s.f.token(t, &s.index) == sl::Result::eOk && t) s.f.marker(m, *t);
}

void open_frame(State& s) {
    if (s.open) return;
    s.open = true;
    if (!s.markers) return;
    sl::FrameToken* t = nullptr;
    if (s.f.token(t, &s.index) != sl::Result::eOk || !t) return;
    const uint64_t t0 = FrameLimiter::now_ns();
    s.f.reflex_sleep(*t);
    g_sleep_ns.store(FrameLimiter::now_ns() - t0, std::memory_order_relaxed);
    s.f.marker(sl::PCLMarker::eSimulationStart, *t);
    s.f.marker(sl::PCLMarker::eSimulationEnd, *t);
    s.f.marker(sl::PCLMarker::eRenderSubmitStart, *t);
}

bool fg_init(State& s, gfx::Device* dev) {
    if (s.fg_tried) return s.fg;
    s.fg_tried = true;
    s.dev = dev;
    if (!s.markers || !dev || !streamline_framegen_available()) return false;
    sl::AdapterInfo ai;
    ai.vkPhysicalDevice = reinterpret_cast<void*>(dev->physical());
    const sl::Result sup = s.f.supported(sl::kFeatureDLSS_G, ai);
    if (sup != sl::Result::eOk && framegen_support_override(static_cast<int>(sup))) {
        NXLOG_W(kTag, "slIsFeatureSupported(DLSS-G) %d: going on ([dlss] frame_gen_any_gpu); the DLSS-G plugin's own state decides",
                static_cast<int>(sup));
    } else if (sup != sl::Result::eOk) {
        NXLOG_W(kTag, "DLSS Frame Generation is not supported here (slIsFeatureSupported %d): off", static_cast<int>(sup));
        return false;
    }
    if (!feature_fn(s, sl::kFeatureDLSS_G, "slDLSSGSetOptions", s.f.fg_set) || !feature_fn(s, sl::kFeatureDLSS_G, "slDLSSGGetState", s.f.fg_get)) return false;
    sl::DLSSGState gs;
    if (s.f.fg_get(sl::ViewportHandle(0u), gs, nullptr) != sl::Result::eOk) {
        NXLOG_W(kTag, "DLSS-G state unavailable: frame generation off");
        return false;
    }
    NXLOG_I(kTag, "DLSS Frame Generation available (status %u, at most %u generated frames, minimum size %u)%s", static_cast<unsigned>(gs.status),
            gs.numFramesToGenerateMax, gs.minWidthOrHeight, fg_setting() == 2 ? ": kept off ([dlss] frame_gen = 2)" : "");
    if (!gs.numFramesToGenerateMax) {
        NXLOG_W(kTag, "DLSS-G reports no supported generated frames: using ordinary presentation");
        return false;
    }
    s.min_dimension = gs.minWidthOrHeight;
    s.fg = true;
    return true;
}

// The F1 page's copy of what frame generation is doing (render_status.h). Only what this module can prove: the setting, the backend's own
// lifecycle, and the presents it counted. "Generating" needs generated frames in a closed window, never a loaded DLL or an option set.
void publish_status(State& s) {
    using render_status::FrameGen;
    FrameGen fg = FrameGen::Unknown;
    bool dlss = false, ffx = false;
    const int mode = fg_setting();
    // Read here as well as in the present path: a game in the background stops presenting, so the present-time value would stay frozen at
    // whatever it was when the window still had the focus. The focus is also cleared here, so a stale Unfocused cannot outlive the alt-tab.
    render_status::Pause pause = s.pause_reason;
    if (!platform::focused()) {
        if (pause == render_status::Pause::None) pause = render_status::Pause::Unfocused;
    } else if (pause == render_status::Pause::Unfocused) {
        pause = render_status::Pause::None;
    }
    if (mode == 0) {
        fg = FrameGen::Off;
    } else if (mode == 3) {
        ffx = true;
        if (!ffx_fg_files()) fg = FrameGen::Missing;
        else if (!ffx_fg_wanted()) fg = FrameGen::Missing;
        else if (s.win_measured && s.win_game > 0 && s.win_shown > s.win_game) fg = FrameGen::Generating;
        else if (s.on) fg = s.win_measured ? FrameGen::Ready : FrameGen::Waiting;
        else fg = s.win_measured ? FrameGen::Ready : FrameGen::Waiting;
    } else {
        dlss = true;
        if (!streamline_framegen_available() || !streamline_framegen_files()) fg = FrameGen::Missing;
        else if (!temporal_dlss_started()) fg = FrameGen::Missing;
        else if (s.fg_tried && !s.fg) fg = FrameGen::Failed;
        else if (!s.fg) fg = FrameGen::Waiting;
        else if (s.win_measured && s.win_game > 0 && s.win_shown > s.win_game) fg = FrameGen::Generating;
        else fg = FrameGen::Ready;
        if (fg == FrameGen::Generating && pause != render_status::Pause::None) fg = FrameGen::Paused;
        // A backend that is set up and would run, held back by the window: this is the pause a player sees by alt-tabbing. Missing and
        // failed are not pauses and keep their own wording.
        if ((fg == FrameGen::Ready || fg == FrameGen::Waiting) && pause != render_status::Pause::None) fg = FrameGen::Paused;
    }
    render_status::set_framegen(fg, dlss, ffx, fg == FrameGen::Paused || fg == FrameGen::Generating ? pause : render_status::Pause::None, s.status,
                                s.win_shown, s.win_game, s.win_game_fps, s.win_shown_fps);
    s.status_ns = FrameLimiter::now_ns();
}

// The page reads a snapshot; four publishes a second are plenty and keep the present path free of string work.
void publish_status_throttled(State& s) {
    const uint64_t now = FrameLimiter::now_ns();
    if (s.status_ns && now - s.status_ns < 250000000ull) return;
    publish_status(s);
}

bool ensure(State& s, gfx::Image& img, VkExtent2D e, VkFormat fmt, const char* name) {
    if (img.image && img.extent.width == e.width && img.extent.height == e.height) return true;
    if (img.image) s.retired.emplace_back(s.index, img);
    img = gfx::Image();
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.name = name;
    std::string why;
    if (s.dev->create_image(d, &img, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s %ux%u: not made (%s)", name, e.width, e.height, why.c_str());
        img = gfx::Image();
        return false;
    }
    return true;
}

void barrier(VkCommandBuffer cb, const VkImageMemoryBarrier2* imgs, uint32_t n) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    di.imageMemoryBarrierCount = n;
    di.pImageMemoryBarriers = imgs;
    vkCmdPipelineBarrier2(cb, &di);
}

sl::Resource resource(const gfx::Image& i) {
    sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(i.image), nullptr, reinterpret_cast<void*>(i.view),
                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
    r.width = i.extent.width;
    r.height = i.extent.height;
    r.nativeFormat = static_cast<uint32_t>(i.format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return r;
}

// The frame's depth and motion into the next pair of our images, flipped to the backbuffer's orientation when asked. Null: not made.
Pair* copy_inputs(State& s, VkCommandBuffer cb, const FramegenInputs& in) {
    Pair& p = s.pair[s.cur];
    s.cur ^= 1u;
    const bool fresh = !p.depth.image || p.depth.extent.width != in.size.width || p.depth.extent.height != in.size.height;
    if (!ensure(s, p.depth, in.size, VK_FORMAT_R32_SFLOAT, "framegen depth") || !ensure(s, p.motion, in.size, VK_FORMAT_R16G16_SFLOAT, "framegen motion")) return nullptr;
    if (fresh) p.init = false;
    VkImageMemoryBarrier2 ib[2] = {};
    for (int i = 0; i < 2; ++i) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib[i].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib[i].oldLayout = p.init ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = i ? p.motion.image : p.depth.image;
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barrier(cb, ib, 2);
    p.init = true;
    const int32_t w = static_cast<int32_t>(in.size.width), h = static_cast<int32_t>(in.size.height);
    VkImageBlit b = {};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {w, h, 1};
    b.dstOffsets[0] = {0, in.flip_y ? h : 0, 0};
    b.dstOffsets[1] = {w, in.flip_y ? 0 : h, 1};
    vkCmdBlitImage(cb, in.depth, VK_IMAGE_LAYOUT_GENERAL, p.depth.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    vkCmdBlitImage(cb, in.motion, VK_IMAGE_LAYOUT_GENERAL, p.motion.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    barrier(cb, nullptr, 0);
    return &p;
}

void set_mode(State& s, bool on) {
    sl::DLSSGOptions o;
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    o.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    o.onErrorCallback = &on_api_error;
    const sl::Result r = s.f.fg_set(sl::ViewportHandle(0u), o);
    if (r != sl::Result::eOk) {
        NXLOG_E(kTag, "slDLSSGSetOptions failed (%d): frame generation off", static_cast<int>(r));
        s.fg = false;
        s.on = false;
        clear_shown_ratio();
        return;
    }
    if (on != s.on) NXLOG_I(kTag, "frame generation %s at frame %u", on ? "on" : "off", s.index);
    s.on = on;
}

void report(State& s) {
    const uint64_t now = FrameLimiter::now_ns();
    if (!s.t0) s.t0 = now;
    if (now - s.t0 < 2000000000ull) return;
    const double sec = static_cast<double>(now - s.t0) * 1e-9;
    double lat = 0, gpu = 0;
    int n = 0;
    if (s.f.reflex_get) {
        sl::ReflexState rs;
        if (s.f.reflex_get(rs) == sl::Result::eOk) {
            for (const sl::ReflexReport& r : rs.frameReport) {
                if (!r.simStartTime || r.gpuRenderEndTime <= r.simStartTime || r.gpuRenderEndTime - r.simStartTime > 1000000) continue;
                lat += static_cast<double>(r.gpuRenderEndTime - r.simStartTime);
                gpu += r.gpuFrameTimeUs;
                ++n;
            }
        }
    }
    NXLOG_I(kTag, "%.1f game fps, %.1f shown fps, generation on %u of %u frames, status %u, last API error %d, Reflex sim start -> GPU end %.2f ms, GPU frame %.2f ms (%d frames)",
            s.n_game / sec, s.n_shown / sec, s.n_on, s.n_game, s.status, g_api_error.load(), n ? lat / n * 1e-3 : 0.0, n ? gpu / n * 1e-3 : 0.0, n);
    set_shown_ratio(s.n_game, s.n_shown, s.n_on);
    s.win_shown = s.n_shown;
    s.win_game = s.n_game;
    s.win_game_fps = s.n_game / sec;
    s.win_shown_fps = s.n_shown / sec;
    s.win_measured = s.n_game > 0;
    publish_status(s);
    if (s.n_extra_inputs || s.n_extra_sr)
        NXLOG_W(kTag, "%u frames had more than one DLSS evaluation (frame generation used the first one's inputs; DLSS SR moved to a new frame index %u times)",
                s.n_extra_inputs, s.n_extra_sr);
    s.t0 = now;
    s.n_game = s.n_shown = s.n_on = s.n_extra_inputs = s.n_extra_sr = 0;
}

}  // namespace

int framegen_mode() { return fg_setting(); }
uint64_t framegen_take_sleep_ns() { return g_sleep_ns.exchange(0, std::memory_order_relaxed); }

double framegen_pace(double game_fps, bool fifo, float refresh_hz) {
    State& s = st();
    bool paused = false;
    double out = game_fps;
    if (fifo && refresh_hz > 0 && framegen_running()) {
        const double half = static_cast<double>(refresh_hz) * 0.5 * 0.9995;
        if (game_fps > 0 && half < game_fps * 0.95) paused = true;
        else out = game_fps > 0 ? std::min(game_fps, half) : half;
    }
    if (paused != s.vsync_paused || (fifo && refresh_hz > 0 && game_fps > 0 && !s.pace_told)) {   // the game's rate known: one line
        s.pace_told = s.pace_told || (fifo && refresh_hz > 0 && game_fps > 0);
        if (paused)
            NXLOG_W(kTag, "frame generation paused: vsync is on and the display's %.0f Hz cannot show two frames for each of the game's %.0f: "
                          "turn vsync off (or use a display of at least %.0f Hz) to use it", refresh_hz, game_fps, game_fps * 2.0);
        else if (fifo && refresh_hz > 0)
            NXLOG_I(kTag, "frame generation with vsync on a %.0f Hz display: the game renders at most %.2f fps (just under half the refresh rate)",
                    refresh_hz, static_cast<double>(refresh_hz) * 0.5 * 0.9995);
    }
    s.vsync_paused = paused;
    return out;
}
bool framegen_vsync_paused() { return st().vsync_paused; }
bool framegen_requested() { return fg_setting() != 0; }
bool framegen_dlss() { return dlss_fg(); }

bool framegen_support_override(int sl_result) {
    static const bool any_gpu = config_get_bool("dlss", "frame_gen_any_gpu", false);
    const auto r = static_cast<sl::Result>(sl_result);
    return any_gpu && (r == sl::Result::eErrorAdapterNotSupported || r == sl::Result::eErrorFeatureNotSupported);
}
bool framegen_running() { return fg_setting() == 3 ? ffx_fg_present_count() > 0 || st().fg : st().fg; }

void* sl_frame_token() {
    if (!temporal_dlss_started() || !base_init()) return nullptr;
    State& s = st();
    sl::FrameToken* t = nullptr;
    if (!dlss_fg()) return s.f.token(t, nullptr) == sl::Result::eOk ? t : nullptr;
    if (s.sr_index == s.index) {
        ++s.n_extra_sr;
        ++s.index;
        s.open = false;
        s.inputs = false;
        open_frame(s);
    }
    s.sr_index = s.index;
    return s.f.token(t, &s.index) == sl::Result::eOk ? t : nullptr;
}

void framegen_inputs(void* gfx_device, VkCommandBuffer cb, const FramegenInputs& in) {
    State& s = st();
    if (fg_setting() == 3) {
        if (!ffx_fg_wanted() || !in.depth || !in.motion || !in.size.width || !in.size.height) return;
        s.dev = static_cast<gfx::Device*>(gfx_device);
        if (s.inputs) {
            ++s.n_extra_inputs;
            return;
        }
        if (Pair* p = copy_inputs(s, cb, in)) {
            s.ffx = FfxFgFrame();
            s.ffx.depth = p->depth.image;
            s.ffx.motion = p->motion.image;
            s.ffx.render = in.size;
            const float fy = in.flip_y ? -1.0f : 1.0f;
            s.ffx.jitter[0] = in.jitter_px[0];
            s.ffx.jitter[1] = in.jitter_px[1] * fy;
            s.ffx.mv_scale[0] = static_cast<float>(in.size.width);
            s.ffx.mv_scale[1] = static_cast<float>(in.size.height) * fy;
            s.ffx.reset = in.reset;
            s.ffx.depth_inverted = in.depth_inverted;
            s.ffx.inputs = true;
            s.inputs = true;
        }
        return;
    }
    if (!dlss_fg() || !temporal_dlss_started() || !base_init()) return;
    if (!fg_init(s, static_cast<gfx::Device*>(gfx_device)) || !in.depth || !in.motion || !in.size.width || !in.size.height) return;
    open_frame(s);
    if (s.inputs) {
        ++s.n_extra_inputs;
        return;
    }
    if (!copy_inputs(s, cb, in)) return;
    Pair& p = s.pair[s.cur ^ 1u];
    const int32_t w = static_cast<int32_t>(in.size.width), h = static_cast<int32_t>(in.size.height);



    sl::FrameToken* t = nullptr;
    if (s.f.token(t, &s.index) != sl::Result::eOk || !t) return;
    const float fy = in.flip_y ? -1.0f : 1.0f;
    sl::Constants c;
    sl::float4x4 id;
    for (int i = 0; i < 4; ++i) id.row[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
    c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = id;
    c.jitterOffset = sl::float2(in.jitter_px[0], in.jitter_px[1] * fy);
    c.mvecScale = sl::float2(1.0f, fy);
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    c.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    c.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    c.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    c.cameraNear = 0.1f;
    c.cameraFar = 1000.0f;
    c.cameraFOV = 1.0f;
    c.cameraAspectRatio = static_cast<float>(w) / static_cast<float>(h);
    c.depthInverted = in.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    const sl::ViewportHandle vp(0u);
    sl::Result r = s.f.constants(c, *t, vp);
    if (r != sl::Result::eOk) {
        NXLOG_E(kTag, "slSetConstants failed (%d)", static_cast<int>(r));
        return;
    }
    sl::Resource dep = resource(p.depth), mv = resource(p.motion);
    const sl::Extent ext{0, 0, in.size.width, in.size.height};
    sl::ResourceTag tags[2] = {sl::ResourceTag(&dep, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
                               sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext)};
    r = s.f.tag(*t, vp, tags, 2, reinterpret_cast<sl::CommandBuffer*>(cb));
    if (r != sl::Result::eOk) {
        NXLOG_E(kTag, "slSetTagForFrame failed (%d)", static_cast<int>(r));
        return;
    }
    s.inputs = true;
}

void framegen_hudless(VkCommandBuffer cb, VkImage image, VkImageView view, VkFormat format, VkExtent2D size) {
    State& s = st();
    if (fg_setting() == 3) {
        if (s.inputs) s.ffx_hudless = image;
        return;
    }
    if (!s.fg || !s.inputs || !image) return;
    sl::FrameToken* t = nullptr;
    if (s.f.token(t, &s.index) != sl::Result::eOk || !t) return;
    sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(image), nullptr, reinterpret_cast<void*>(view), static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
    r.width = size.width;
    r.height = size.height;
    r.nativeFormat = static_cast<uint32_t>(format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const sl::Extent ext{0, 0, size.width, size.height};
    sl::ResourceTag tag(&r, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &ext);
    const sl::Result res = s.f.tag(*t, sl::ViewportHandle(0u), &tag, 1, reinterpret_cast<sl::CommandBuffer*>(cb));
    static bool told = false;
    if (res != sl::Result::eOk && !told) {
        told = true;
        NXLOG_E(kTag, "HUD-less tag failed (%d)", static_cast<int>(res));
    }
}

namespace {
void picture_rect(VkExtent2D backbuffer, VkExtent2D src, const int32_t crop[4], bool keep_aspect, int32_t rect[4]) {
    double cw = src.width, ch = src.height;
    if (crop && crop[2] > 0 && crop[3] > 0) {
        cw = std::min<double>(crop[2], src.width);
        ch = std::min<double>(crop[3], src.height);
    }
    const double dw = backbuffer.width, dh = backbuffer.height;
    double pw = dw, ph = dh;
    if (keep_aspect && cw > 0 && ch > 0) {
        if (cw / ch > dw / dh) ph = dw * ch / cw;
        else pw = dh * cw / ch;
    }
    rect[0] = static_cast<int32_t>(std::lround((dw - pw) * 0.5));
    rect[1] = static_cast<int32_t>(std::lround((dh - ph) * 0.5));
    rect[2] = static_cast<int32_t>(std::lround(pw));
    rect[3] = static_cast<int32_t>(std::lround(ph));
}

void ffx_present_begin(State& s, VkCommandBuffer cb, VkExtent2D backbuffer, const int32_t rect[4], bool again) {
    const uint64_t now = FrameLimiter::now_ns();
    FfxFgFrame f = s.ffx;
    f.frame_ms = s.last_present_ns ? static_cast<float>(std::clamp(static_cast<double>(now - s.last_present_ns) * 1e-6, 1.0, 100.0)) : 16.7f;
    s.last_present_ns = now;
    const bool want = s.inputs && !again && !s.vsync_paused;
    s.streak = want ? s.streak + 1 : 0;
    f.enable = want && s.streak >= 2;
    f.inputs = s.inputs;
    f.hudless = s.ffx_hudless;
    f.bb = backbuffer;
    std::copy(rect, rect + 4, f.rect);
    if (!ffx_fg_frame(cb, f)) return;
    if (f.enable != s.on) NXLOG_I(kTag, "FSR 3 frame generation %s at frame %u", f.enable ? "on" : "off", s.index);
    s.on = f.enable;
}
}  // namespace

void framegen_present_begin(VkCommandBuffer cb, VkExtent2D backbuffer, VkExtent2D src, const int32_t crop[4], bool keep_aspect, bool again) {
    if (fg_setting() == 3) {
        if (!ffx_fg_wanted()) return;
        int32_t rect[4];
        picture_rect(backbuffer, src, crop, keep_aspect, rect);
        ffx_present_begin(st(), cb, backbuffer, rect, again);
        return;
    }
    if (!dlss_fg() || !temporal_dlss_started() || !base_init()) return;
    State& s = st();
    open_frame(s);
    if (s.fg) {
        int32_t rect[4];
        picture_rect(backbuffer, src, crop, keep_aspect, rect);
        const bool small = backbuffer.width < s.min_dimension || backbuffer.height < s.min_dimension;
        const bool want = s.inputs && !again && fg_setting() == 1 && !s.vsync_paused && !small;
        // Reporting only: a game in the background stops presenting new frames, and the page says which of the two it was.
        const bool unfocused = !platform::focused();
        s.pause_reason = want ? render_status::Pause::None
                              : s.vsync_paused ? render_status::Pause::Vsync
                              : small ? render_status::Pause::SmallBackbuffer
                              : unfocused ? render_status::Pause::Unfocused
                              : render_status::Pause::NoInputs;
        s.streak = want ? s.streak + 1 : 0;
        const bool on = want && s.streak >= 2;
        const bool moved = backbuffer.width != s.bb.width || backbuffer.height != s.bb.height || !std::equal(rect, rect + 4, s.rect);
        if (moved && s.on) set_mode(s, false);
        s.bb = backbuffer;
        std::copy(rect, rect + 4, s.rect);
        if (on && (moved || !s.on)) set_mode(s, true);
        else if (!on && s.on) set_mode(s, false);
        if (s.on) {   // tag the full rect too, clearing any previous letterbox extent
            sl::FrameToken* t = nullptr;
            const sl::Extent sub{static_cast<uint32_t>(rect[1]), static_cast<uint32_t>(rect[0]), static_cast<uint32_t>(rect[2]), static_cast<uint32_t>(rect[3])};
            sl::ResourceTag bt(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &sub);
            if (s.f.token(t, &s.index) == sl::Result::eOk && t) s.f.tag(*t, sl::ViewportHandle(0u), &bt, 1, reinterpret_cast<sl::CommandBuffer*>(cb));
        }
    }
    if (s.markers) {
        marker(s, sl::PCLMarker::eRenderSubmitEnd);
        marker(s, sl::PCLMarker::ePresentStart);
    }
}

void framegen_present_end() {
    State& s = st();
    if (fg_setting() == 3) {
        if (!ffx_fg_wanted()) {
            clear_shown_ratio();
            publish_status_throttled(s);
            return;
        }
        ++s.n_game;
        s.n_on += s.on ? 1 : 0;
        const uint64_t now = FrameLimiter::now_ns();
        if (!s.t0) {
            s.t0 = now;
            s.ffx_count0 = ffx_fg_present_count();
        } else if (now - s.t0 >= 2000000000ull) {
            const double sec = static_cast<double>(now - s.t0) * 1e-9;
            const uint64_t c = ffx_fg_present_count();
            NXLOG_I(kTag, "FSR 3: %.1f game fps, %.1f shown fps (swapchain presents), generation on %u of %u frames%s", s.n_game / sec,
                    static_cast<double>(c - s.ffx_count0) / sec, s.n_on, s.n_game,
                    s.n_extra_inputs ? ", some frames had more than one DLSS evaluation" : "");
            set_shown_ratio(s.n_game, static_cast<double>(c - s.ffx_count0), s.n_on);
            s.win_shown = static_cast<uint32_t>(c - s.ffx_count0);
            s.win_game = s.n_game;
            s.win_game_fps = s.n_game / sec;
            s.win_shown_fps = static_cast<double>(c - s.ffx_count0) / sec;
            s.win_measured = s.n_game > 0;
            publish_status(s);
            s.t0 = now;
            s.ffx_count0 = c;
            s.n_game = s.n_on = s.n_extra_inputs = 0;
        }
    } else {
        if (!temporal_dlss_started() || !base_init()) {
            clear_shown_ratio();
            publish_status_throttled(s);
            return;
        }
        if (!s.fg) {   // DLSS-G not running or stopped (slDLSSGSetOptions failed): no more reports
            clear_shown_ratio();
            publish_status_throttled(s);
        }
    }
    if (s.markers) marker(s, sl::PCLMarker::ePresentEnd);
    if (s.fg) {
        uint32_t shown = 0;
        sl::DLSSGState gs;
        if (s.f.fg_get(sl::ViewportHandle(0u), gs, nullptr) == sl::Result::eOk) {
            shown = gs.numFramesActuallyPresented;   // report measured presents, never invent a frame
            s.status = static_cast<uint32_t>(gs.status);
        }
        s.n_on += s.on ? 1 : 0;
        ++s.n_game;
        s.n_shown += shown;
        report(s);
        publish_status_throttled(s);
    }
    for (size_t i = 0; i < s.retired.size();) {
        if (s.index - s.retired[i].first > 8) {
            s.dev->destroy_image(s.retired[i].second);
            s.retired.erase(s.retired.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    ++s.index;
    s.open = false;
    s.inputs = false;
    s.ffx = FfxFgFrame();
    s.ffx_hudless = VK_NULL_HANDLE;
    open_frame(s);
}

void framegen_swapchain_reset() {
    State& s = st();
    if (s.on && s.f.fg_set) set_mode(s, false);
    if (s.fg && s.f.tag) {
        sl::FrameToken* t = nullptr;
        sl::ResourceTag tags[] = {
            {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent}};
        if (s.f.token(t, &s.index) == sl::Result::eOk && t)
            s.f.tag(*t, sl::ViewportHandle(0u), tags, 4, nullptr);
    }
    s.streak = 0;
    s.inputs = false;
    s.bb = {};
}

void framegen_shutdown(void* gfx_device) {
    State& s = st();
    if (s.dev != gfx_device) return;
    framegen_swapchain_reset();
    for (auto& p : s.pair) {
        s.dev->destroy_image(p.depth);
        s.dev->destroy_image(p.motion);
    }
    for (auto& p : s.retired) s.dev->destroy_image(p.second);
    s = State();
    clear_shown_ratio();
}

float framegen_shown_ratio() { return g_shown_ratio.load(std::memory_order_relaxed); }

}  // namespace nxrt::extras
