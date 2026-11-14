// SPDX-License-Identifier: GPL-3.0-or-later
// Low-latency modes (include/nxrt/lowlat.h): NVIDIA Reflex (VK_NV_low_latency2) and AMD Anti-Lag 2 (VK_AMD_anti_lag).
#include "nxrt/lowlat.h"
#include "extras/temporal.h"
#include "extras/framegen_ffx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/game.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "lowlat";
std::atomic<LowLatency*> g_instance{nullptr};
std::atomic<uint64_t (*)()> g_backlog_fn{nullptr};
std::atomic<bool (*)(int, uint64_t)> g_gpu_wait_fn{nullptr};
std::atomic<uint64_t> g_boundary_paced_ns{0};
std::atomic<uint64_t> g_presents{0};
uint64_t g_boundary_presents = ~0ull;     // main thread only
std::atomic<double> g_cap_fps{0};

uint64_t now_us() { return FrameLimiter::now_ns() / 1000ull; }
double ms_since(uint64_t t_ns) { return static_cast<double>(FrameLimiter::now_ns() - t_ns) * 1e-6; }
}  // namespace

struct LowLatency::Impl {
    gfx::Device* dev = nullptr;
    LatencySettings cfg;
    VkSwapchainLatencyCreateInfoNV sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};

    // swapchain and sleep mode
    std::mutex mu;
    std::condition_variable cv;               // notified by present_begin: a frame left the in-flight queue
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool mode_set = false;
    double cap_fps = 0;
    VkSemaphore sleep_sem = VK_NULL_HANDLE;
    uint64_t sleep_value = 0;

    // frame ids: sim_begin takes the next, sim_end queues it, present_begin pops the oldest
    std::atomic<uint64_t> sim_id{0};
    std::deque<uint64_t> pending;            // ids simulated and not yet presented (guarded by mu)
    std::atomic<uint64_t> last_present_ns{0};  // when the last present started: the in-flight wait is skipped while nothing is being presented
    uint64_t present_id = 0;                  // id of the present in progress (render thread)
    uint64_t present_sim = 0;                 // simulation id shown by the present in progress (0: none)
    uint64_t last_present_id = 0;
    bool submit_marked = false;               // RENDERSUBMIT_START was set for the frame in progress

    // measurement
    std::atomic<uint64_t> input_ns_for[64] = {};   // id % 64 -> time of INPUT_SAMPLE
    std::atomic<uint64_t> frames{0};
    double sleep_ms_avg = 0, in2present_ema = 0, in2gpu_ema = 0, sim2present_ema = 0;
    std::atomic<uint64_t> reports{0};
    uint64_t last_report_us = 0, last_log_ns = 0;
    uint64_t report_seen_id = 0;
    bool have_ema = false, have_gpu_ema = false;

    void set_mode_locked() {
        if (!swapchain) return;
        if (backend_reflex()) {
            VkLatencySleepModeInfoNV mi = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
            mi.lowLatencyMode = live_on ? VK_TRUE : VK_FALSE;
            mi.lowLatencyBoost = cfg.mode == LatencyMode::Boost ? VK_TRUE : VK_FALSE;
            mi.minimumIntervalUs = cap_fps > 0 ? static_cast<uint32_t>(1e6 / cap_fps) : 0;
            const VkResult r = vkSetLatencySleepModeNV(dev->device(), swapchain, &mi);
            mode_set = r == VK_SUCCESS;
            if (!mode_set) NXLOG_W(kTag, "vkSetLatencySleepModeNV failed (%d)", static_cast<int>(r));
            else NXLOG_I(kTag, "Reflex sleep mode set: low latency %s, boost %s, interval %u us", live_on ? "on" : "off", mi.lowLatencyBoost ? "on" : "off", mi.minimumIntervalUs);
        }
    }
    bool backend_reflex() const { return reflex; }
    bool reflex = false, antilag = false;
    bool live_on = true;                      // [latency] ab_toggle_s: the mode is switched on and off while running, to compare both states in one run
    double ab_period_s = 0;
    uint64_t ab_next_ns = 0, ab_since_ns = 0;
    bool paced = false;                       // the mode is on: the frames_ahead wait runs (also on GPUs with neither extension)
    bool active = false;                      // markers and the host measurement run (enabled, or log_timings asks for the numbers)
    bool running() const { return active || ft::on(); }     // the frame trace (S18 latency probe) also keeps the frame ids

    // `swapchain` and `mode_set` change only with the queue lock held (and mu), so the Reflex calls read them under the queue lock they take
    // anyway: the renderer destroys a swapchain under that lock too, and a handle read before it could be gone by the call.
    void marker(VkLatencyMarkerNV m, uint64_t id) {
        if (!reflex) return;
        std::lock_guard<std::mutex> ql(dev->queue_mutex());      // never called with the queue lock held
        if (!swapchain || !mode_set) return;
        VkSetLatencyMarkerInfoNV mi = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
        mi.presentID = id;
        mi.marker = m;
        vkSetLatencyMarkerNV(dev->device(), swapchain, &mi);
    }

    void antilag_update(VkAntiLagStageAMD stage, uint64_t id) {
        VkAntiLagPresentationInfoAMD pi = {VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD};
        pi.stage = stage;
        pi.frameIndex = id;
        VkAntiLagDataAMD d = {VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD};
        d.mode = VK_ANTI_LAG_MODE_ON_AMD;
        d.maxFPS = cap_fps > 0 ? static_cast<uint32_t>(cap_fps) : 0;
        d.pPresentationInfo = &pi;
        vkAntiLagUpdateAMD(dev->device(), &d);
    }

    void poll_reports() {
        if (!reflex) return;
        const uint64_t t = now_us();
        if (t - last_report_us < 200000) return;       // five polls a second are plenty
        last_report_us = t;
        VkGetLatencyMarkerInfoNV gi = {VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
        gi.timingCount = 0;
        gi.pTimings = nullptr;
        {
            std::lock_guard<std::mutex> ql(dev->queue_mutex());
            if (!swapchain || !mode_set) return;
            vkGetLatencyTimingsNV(dev->device(), swapchain, &gi);
        }
        if (gi.timingCount == 0) return;
        std::vector<VkLatencyTimingsFrameReportNV> v(gi.timingCount);
        for (auto& f : v) f = {VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
        gi.pTimings = v.data();
        {
            std::lock_guard<std::mutex> ql(dev->queue_mutex());
            if (!swapchain || !mode_set) return;      // destroyed in between: the next swapchain has its own reports
            vkGetLatencyTimingsNV(dev->device(), swapchain, &gi);
        }
        double sum_gpu = 0, sum_sp = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < gi.timingCount; ++i) {
            const auto& f = v[i];
            if (f.presentID <= report_seen_id) continue;
            if (!f.inputSampleTimeUs || !f.gpuRenderEndTimeUs || f.gpuRenderEndTimeUs < f.inputSampleTimeUs) continue;
            sum_gpu += static_cast<double>(f.gpuRenderEndTimeUs - f.inputSampleTimeUs) * 1e-3;
            if (f.simStartTimeUs && f.presentEndTimeUs >= f.simStartTimeUs) sum_sp += static_cast<double>(f.presentEndTimeUs - f.simStartTimeUs) * 1e-3;
            report_seen_id = std::max(report_seen_id, f.presentID);
            ++n;
        }
        if (n) {
            const double g = sum_gpu / n, sp = sum_sp / n;
            in2gpu_ema = have_gpu_ema ? in2gpu_ema * 0.9 + g * 0.1 : g;
            sim2present_ema = have_gpu_ema ? sim2present_ema * 0.9 + sp * 0.1 : sp;
            have_gpu_ema = true;
            reports += n;
        }
    }
};

// The default is main, except for a game that paces itself by the present interval (game::Info::present_interval_hz, Fortune's Weave): its
// rate is held in the present (nvn_present.cpp), and its frame hook only pumps play mode.
bool pace_in_main() {
    static const bool v = config_get_str("latency", "pace", game::info().present_interval_hz > 0 ? "present" : "main") != "present";
    return v;
}
void pace_note_present() { g_presents.fetch_add(1, std::memory_order_relaxed); }
bool pace_boundary_due() {
    const uint64_t n = g_presents.load(std::memory_order_relaxed);
    if (n == g_boundary_presents) return false;
    g_boundary_presents = n;
    return true;
}
void pace_boundary_done() { g_boundary_paced_ns.store(FrameLimiter::now_ns(), std::memory_order_relaxed); }
bool pace_boundary_consume() {
    // A recent boundary wait, not a count: presents and boundaries need not pair up (loading screens present without a boundary, the main loop
    // can run a boundary without a present), and a count that drifts makes the presenting thread take a second slot of the same schedule.
    const uint64_t t = g_boundary_paced_ns.load(std::memory_order_relaxed);
    return t != 0 && FrameLimiter::now_ns() - t < 250000000ull;
}

void lowlat_set_backlog_fn(uint64_t (*fn)()) { g_backlog_fn.store(fn); }
void lowlat_set_gpu_wait_fn(bool (*fn)(int, uint64_t)) { g_gpu_wait_fn.store(fn); }
void lowlat_note_cap(double fps) {
    g_cap_fps.store(fps > 0 ? fps : 0);
    if (LowLatency* l = g_instance.load(std::memory_order_acquire)) l->set_cap_fps(fps);
}
LowLatency* lowlat() { return g_instance.load(std::memory_order_acquire); }
void lowlat_set(LowLatency* l) { g_instance.store(l, std::memory_order_release); }

LowLatency::LowLatency(gfx::Device& dev) : impl_(new Impl) {
    Impl& s = *impl_;
    s.dev = &dev;
    s.cap_fps = g_cap_fps.load();
    s.cfg = settings().latency;
    if (extras::streamline_framegen_available()) {
        if (s.cfg.mode != LatencyMode::Off)
            NXLOG_I(kTag, "DLSS-G owns Reflex: disabling the runtime latency backend");
        s.cfg.mode = LatencyMode::Off;
    } else if (extras::ffx_fg_wanted()) {
        // FSR 3 frame generation's swapchain (framegen_ffx.cpp) paces and presents on its own queues: Reflex markers and sleep on that
        // swapchain (or Anti-Lag 2 without AMD's frame generation hooks) fight its pacing, as with DLSS-G.
        if (s.cfg.mode != LatencyMode::Off)
            NXLOG_I(kTag, "FSR 3 frame generation is on: disabling the runtime latency backend");
        s.cfg.mode = LatencyMode::Off;
    }
    const gfx::DeviceFeatures& f = dev.features();
    if (s.cfg.mode != LatencyMode::Off) {
        if (f.low_latency2) { s.reflex = true; backend_ = Backend::Reflex; }
        else if (f.anti_lag) { s.antilag = true; backend_ = Backend::AntiLag; }
        else NXLOG_W(kTag, "low latency requested, but the GPU has neither VK_NV_low_latency2 (with present id) nor VK_AMD_anti_lag: off");
    }
    // Boost is a Reflex setting; with Anti-Lag it behaves as On. Auto and On both use whichever backend exists.
    enabled_ = backend_ != Backend::None;
    s.paced = s.cfg.mode != LatencyMode::Off || pace_in_main();
    s.ab_period_s = config_get_double("latency", "ab_toggle_s", 0.0);     // test aid, not a user setting
    if (s.ab_period_s > 0) s.ab_next_ns = FrameLimiter::now_ns() + static_cast<uint64_t>(s.ab_period_s * 1e9);
    s.active = enabled_ || s.paced || s.cfg.log_timings;
    NXLOG_I(kTag, "frame pacing: %s (frames_ahead %d)", pace_in_main() ? "limiter at the frame boundary" : "limiter in the present call", s.cfg.frames_ahead);
    if (enabled_) {
        s.sci.latencyModeEnable = s.reflex ? VK_TRUE : VK_FALSE;
        if (s.reflex) {
            VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
            if (vkCreateSemaphore(dev.device(), &sci, nullptr, &s.sleep_sem) != VK_SUCCESS) {
                NXLOG_W(kTag, "cannot create the Reflex sleep semaphore: off");
                s.reflex = false;
                enabled_ = false;
                s.active = s.paced || s.cfg.log_timings;
                backend_ = Backend::None;
            }
        }
    }
    NXLOG_I(kTag, "low latency: %s (mode %d, backend %s)", enabled_ ? "on" : "off", static_cast<int>(s.cfg.mode), backend_name());
}

LowLatency::~LowLatency() {
    if (g_instance.load() == this) g_instance.store(nullptr);
    if (impl_) {
        if (impl_->sleep_sem) {
            impl_->dev->wait_idle();
            vkDestroySemaphore(impl_->dev->device(), impl_->sleep_sem, nullptr);
        }
        delete impl_;
    }
}

const char* LowLatency::backend_name() const {
    switch (backend_) {
        case Backend::Reflex: return "NVIDIA Reflex (VK_NV_low_latency2)";
        case Backend::AntiLag: return "AMD Anti-Lag 2 (VK_AMD_anti_lag)";
        default: return "none";
    }
}

void LowLatency::prepare_swapchain(VkSwapchainCreateInfoKHR* ci) {
    if (!enabled_ || !impl_->reflex || !ci) return;
    impl_->sci.pNext = const_cast<void*>(ci->pNext);
    ci->pNext = &impl_->sci;
}

void LowLatency::swapchain_created(VkSwapchainKHR sc) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> ql(s.dev->queue_mutex());      // the layer wants calls on one swapchain from different threads kept apart (lock order: queue, then mu)
    std::lock_guard<std::mutex> lk(s.mu);
    s.swapchain = sc;
    s.mode_set = false;
    if (enabled_) s.set_mode_locked();
}

// Called with the queue lock held, right before vkDestroySwapchainKHR under the same lock (vk_renderer.cpp): no Reflex call can be using it.
void LowLatency::swapchain_destroyed(VkSwapchainKHR sc) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.swapchain == sc) {
        s.swapchain = VK_NULL_HANDLE;
        s.mode_set = false;
    }
}

void LowLatency::set_cap_fps(double fps) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> ql(s.dev->queue_mutex());
    std::lock_guard<std::mutex> lk(s.mu);
    s.cap_fps = fps > 0 ? fps : 0;
    if (enabled_) s.set_mode_locked();
}

void LowLatency::wait_in_flight() {
    if (!impl_->running()) return;
    Impl& s = *impl_;
    std::unique_lock<std::mutex> lk(s.mu);
    if (s.paced && s.live_on && s.cfg.frames_ahead >= 0) {
        const size_t limit = static_cast<size_t>(s.cfg.frames_ahead);
        // Nothing presented for a while (loading screen, minimised window, movie): there is no queue to wait for, and the frames counted so far will
        // never be popped. Forget them instead of waiting 100 ms per frame.
        const uint64_t lp = s.last_present_ns.load(std::memory_order_relaxed);
        if (!lp || FrameLimiter::now_ns() - lp > 250000000ull) { s.pending.clear(); return; }
        if (!s.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return s.pending.size() <= limit; })) s.pending.clear();
    }
}

void LowLatency::sim_begin() {
    if (!impl_->running()) return;
    Impl& s = *impl_;
    const uint64_t id = s.sim_id.fetch_add(1) + 1;
    const uint64_t t0 = FrameLimiter::now_ns();
    if (s.ab_period_s > 0 && t0 >= s.ab_next_ns) {       // A/B test mode: flip the mode, forget the averages
        std::lock_guard<std::mutex> ql(s.dev->queue_mutex());
        std::lock_guard<std::mutex> lk(s.mu);
        s.live_on = !s.live_on;
        s.ab_next_ns = t0 + static_cast<uint64_t>(s.ab_period_s * 1e9);
        s.ab_since_ns = t0;
        s.have_ema = s.have_gpu_ema = false;
        s.set_mode_locked();
    }
    wait_in_flight();      // frames_ahead: the simulation of a frame starts only when at most that many earlier frames are still on their way to the screen
    {   // [latency] gpu_frames_ahead (lowlat.h): the GPU's own queue is bounded too, right before the input is sampled
        static const int gpu_back = static_cast<int>(std::clamp<int64_t>(config_get_int("latency", "gpu_frames_ahead", 1), -1, 3));
        const uint64_t lp = s.last_present_ns.load(std::memory_order_relaxed);
        if (gpu_back >= 0 && s.paced && s.live_on && lp && FrameLimiter::now_ns() - lp < 250000000ull)
            if (auto fn = g_gpu_wait_fn.load()) fn(gpu_back, 50ull * 1000 * 1000);
    }
    if (s.reflex && s.live_on) {
        VkLatencySleepInfoNV si = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
        si.signalSemaphore = s.sleep_sem;
        VkResult sr = VK_NOT_READY;
        {
            std::lock_guard<std::mutex> ql(s.dev->queue_mutex());     // returns at once; the wait below is what blocks
            if (s.swapchain && s.mode_set) {                          // read under the queue lock: the swapchain is destroyed under it
                si.value = ++s.sleep_value;
                sr = vkLatencySleepNV(s.dev->device(), s.swapchain, &si);
            }
        }
        if (sr == VK_SUCCESS) {
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &s.sleep_sem;
            wi.pValues = &si.value;
            vkWaitSemaphores(s.dev->device(), &wi, 100ull * 1000 * 1000);   // never longer than 100 ms, whatever the driver does
        }
    } else if (s.antilag && s.live_on) {
        s.antilag_update(VK_ANTI_LAG_STAGE_INPUT_AMD, id);
    }
    const double slept = ms_since(t0);
    s.sleep_ms_avg = s.sleep_ms_avg * 0.95 + slept * 0.05;
    s.marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (ft::on()) {      // latency probe: frames simulated and not yet presented (this one excluded), GPU submissions not yet finished
            ft::mark("lat.inflight", static_cast<uint32_t>(s.pending.size()));
            if (auto fn = g_backlog_fn.load()) ft::mark("lat.gpuq", static_cast<uint32_t>(fn()));
            ft::mark("lat.sim", static_cast<uint32_t>(id));
        }
        s.pending.push_back(id);               // in flight until the present that shows it
        while (s.pending.size() > 16) s.pending.pop_front();
    }
    s.frames++;
    s.poll_reports();
}

void LowLatency::input_sample() {
    if (!impl_->running()) return;
    Impl& s = *impl_;
    const uint64_t id = s.sim_id.load();
    s.input_ns_for[id % 64].store(FrameLimiter::now_ns(), std::memory_order_relaxed);
    ft::mark("lat.input", static_cast<uint32_t>(id));
    s.marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

bool LowLatency::paces_frames() const {
    static const bool keep_limiter = config_get_bool("latency", "reflex_limiter", false);
    const Impl& s = *impl_;
    if (keep_limiter || !s.reflex || !enabled_) return false;
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(s.mu));
    return s.mode_set && s.live_on && s.cap_fps > 0;
}

void LowLatency::sim_end() {
    // SIMULATION_END is set by submit_begin(): the host render thread starting on the frame's commands is the first moment the simulation is known to be done.
}

void LowLatency::submit_begin() {
    if (!impl_->running()) return;
    Impl& s = *impl_;
    if (s.submit_marked) return;
    s.submit_marked = true;
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        id = s.pending.empty() ? s.last_present_id + 1 : s.pending.front();
    }
    s.marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
    s.marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
}

uint64_t LowLatency::present_begin() {
    if (!impl_->running()) return 0;
    Impl& s = *impl_;
    uint64_t sim = 0;                            // the simulation id this present shows; 0 = a present with no simulation behind it (menus, loading)
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.pending.empty()) {
            sim = s.pending.front();
            s.pending.pop_front();
        }
    }
    s.cv.notify_all();
    // The id for VkPresentIdKHR and the Reflex markers must increase; the probe marks use the simulation id (or 0), so a present without a
    // simulation does not shift every later frame's id.
    const uint64_t id = std::max(sim, s.last_present_id + 1);
    s.present_id = id;
    s.present_sim = sim;
    s.last_present_id = id;
    s.last_present_ns.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
    ft::mark("lat.present", static_cast<uint32_t>(sim));
    s.marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    s.marker(VK_LATENCY_MARKER_PRESENT_START_NV, id);
    if (s.antilag) s.antilag_update(VK_ANTI_LAG_STAGE_PRESENT_AMD, id);
    s.submit_marked = false;
    return id;
}

uint32_t LowLatency::present_sim_id() const { return static_cast<uint32_t>(impl_->present_sim); }

void LowLatency::present_end() {
    if (!impl_->running()) return;
    Impl& s = *impl_;
    const uint64_t id = s.present_id;
    s.marker(VK_LATENCY_MARKER_PRESENT_END_NV, id);
    ft::mark("lat.present_end", static_cast<uint32_t>(s.present_sim));
    // The input time is kept by simulation id. The present id runs ahead of it once a present had no simulation behind it (loading, some
    // menus: present_begin), so looking it up by the present id read another frame's time, up to 63 frames old: a steady 420 ms at
    // 120 fps on a map in a user's log, 45 ms in the menu before the first load. A present with no simulation has no input time.
    const uint64_t sim = s.present_sim;
    const uint64_t in_ns = sim ? s.input_ns_for[sim % 64].load(std::memory_order_relaxed) : 0;
    if (in_ns) {
        const double d = ms_since(in_ns);
        if (d >= 0 && d < 2000) {
            s.in2present_ema = s.have_ema ? s.in2present_ema * 0.95 + d * 0.05 : d;
            s.have_ema = true;
        }
    }
    if (s.cfg.log_timings) {
        const uint64_t n = FrameLimiter::now_ns();
        if (n - s.last_log_ns > 1000000000ull) {
            s.last_log_ns = n;
            NXLOG_I(kTag, "%s: state %s since %.1f s: input->present %.2f ms, sleep %.2f ms, input->gpu end %.2f ms, sim->present %.2f ms (%llu driver reports)", backend_name(),
                    s.live_on ? "on" : "off", s.ab_since_ns ? static_cast<double>(n - s.ab_since_ns) * 1e-9 : 0.0, s.in2present_ema, s.sleep_ms_avg, s.in2gpu_ema, s.sim2present_ema,
                    static_cast<unsigned long long>(s.reports.load()));
        }
    }
}

LowLatency::Stats LowLatency::stats() const {
    const Impl& s = *impl_;
    Stats st;
    st.backend = backend_;
    st.enabled = enabled_;
    st.frames = s.frames.load();
    st.sleep_ms_avg = s.sleep_ms_avg;
    st.input_to_present_ms = s.in2present_ema;
    st.input_to_gpu_ms = s.in2gpu_ema;
    st.sim_to_present_ms = s.sim2present_ema;
    st.reports = s.reports.load();
    return st;
}

}  // namespace nxrt::extras
